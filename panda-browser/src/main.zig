// ============================================================================
// FILE: panda-browser/src/main.zig
// Standalone Zig entry point, ArenaAllocator lifecycle, HTTP & CDP server
// ============================================================================
const std = @import("std");
const opts = @import("build_options");

// C-ABI declarations matching src/engine_bridge.cpp
extern "C" {
const PandaParseStats = extern struct {
text_len: usize,
title_len: usize,
link_count: usize,
script_count: usize,
parse_us: u32,
};

fn cpp_engine_init(enable_simd: bool, max_pages: u32) callconv(.c) i32;
fn cpp_parse_document(
    html_ptr: [*]const u8,
    html_len: usize,
    title_buf: [*]u8,
    title_cap: usize,
    text_buf: [*]u8,
    text_cap: usize,
) callconv(.c) PandaParseStats;
fn cpp_eval_script(
    script_ptr: [*]const u8,
    script_len: usize,
    out_buf: [*]u8,
    out_cap: usize,
) callconv(.c) usize;
fn cpp_engine_shutdown() callconv(.c) void;


}

pub fn main() !void {
var gpa = std.heap.GeneralPurposeAllocator(.{}){};
defer _ = gpa.deinit();
const base_alloc = gpa.allocator();

const args = try std.process.argsAlloc(base_alloc);
defer std.process.argsFree(base_alloc, args);

if (cpp_engine_init(opts.use_simd, opts.max_pages) != 0) {
    return error.CppEngineInitFailed;
}
defer cpp_engine_shutdown();

if (args.len < 2 or std.mem.eql(u8, args[1], "--info")) {
    printBuildInfo();
    return;
}

if (std.mem.eql(u8, args[1], "--eval")) {
    if (args.len < 3) {
        std.debug.print("Usage: panda-browser --eval \"<expression>\"\n", .{});
        return;
    }
    try evalInlineScript(base_alloc, args[2]);
    return;
}

if (std.mem.eql(u8, args[1], "--cdp")) {
    if (!opts.use_cdp) {
        std.debug.print("Error: Recompile with -Duse-cdp=true to enable CDP server.\n", .{});
        return;
    }
    const port: u16 = if (args.len >= 3) try std.fmt.parseInt(u16, args[2], 10) else 9222;
    try runCdpServer(port);
    return;
}

try processTargetPage(args[1]);


}

fn printBuildInfo() void {
const stdout = std.fs.File.stdout().deprecatedWriter();
stdout.print(
\panda-browser (Zig + C++20 Hybrid Headless Engine)
\Active Gentoo-Style USE Flags:
\  use-quickjs : {}
\  use-v8      : {}
\  use-simd    : {}
\  use-cdp     : {}
\  use-net     : {}
\  max-pages   : {d}
\  arena-kb    : {d} KB
\
\Usage:
\  panda-browser https://example.com
\  panda-browser "TestHello"
\  panda-browser --eval "1 + 1"
\  panda-browser --cdp 9222
\
, .{
opts.use_quickjs,
opts.use_v8,
opts.use_simd,
opts.use_cdp,
opts.use_net,
opts.max_pages,
opts.arena_kb,
}) catch {};
}

fn evalInlineScript(alloc: std.mem.Allocator, script_src: []const u8) !void {
const out_buf = try alloc.alloc(u8, 4096);
defer alloc.free(out_buf);

const written = cpp_eval_script(
    script_src.ptr,
    script_src.len,
    out_buf.ptr,
    out_buf.len,
);

const stdout = std.fs.File.stdout().deprecatedWriter();
try stdout.print("{s}\n", .{out_buf[0..written]});


}

fn processTargetPage(target: []const u8) !void {
// Per-page ArenaAllocator: everything allocated for this page is freed at once
var arena = std.heap.ArenaAllocator.init(std.heap.page_allocator);
defer arena.deinit();
const alloc = arena.allocator();

var html_bytes: []const u8 = target;

if (std.mem.startsWith(u8, target, "http://") or std.mem.startsWith(u8, target, "https://")) {
    if (!opts.use_net) {
        std.debug.print("Error: Recompile with -Duse-net=true to fetch URLs.\n", .{});
        return;
    }
    var client = std.http.Client{ .allocator = alloc };
    defer client.deinit();

    var body_writer = std.Io.Writer.Allocating.init(alloc);
    const res = try client.fetch(.{
        .location = .{ .url = target },
        .response_writer = &body_writer.writer,
    });
    if (res.status != .ok) {
        std.debug.print("HTTP Status: {d}\n", .{@intFromEnum(res.status)});
    }
    html_bytes = body_writer.written();
}

const title_buf = try alloc.alloc(u8, 1024);
const reserve_len = @max(html_bytes.len + 64, @as(usize, opts.arena_kb) * 1024);
const text_buf = try alloc.alloc(u8, reserve_len);

const stats = cpp_parse_document(
    html_bytes.ptr,
    html_bytes.len,
    title_buf.ptr,
    title_buf.len,
    text_buf.ptr,
    text_buf.len,
);

const stdout = std.fs.File.stdout().deprecatedWriter();
try stdout.print(
    \\Title   : {s}
    \\Links   : {d} | Scripts: {d} | ParseTime: {d} us
    \\Text    : {s}
    \\
, .{
    title_buf[0..stats.title_len],
    stats.link_count,
    stats.script_count,
    stats.parse_us,
    text_buf[0..stats.text_len],
});


}

fn runCdpServer(port: u16) !void {
const addr = try std.net.Address.parseIp("127.0.0.1", port);
var server = try addr.listen(.{ .reuse_address = true });
defer server.deinit();

std.debug.print("panda-browser CDP JSON-RPC endpoint listening on 127.0.0.1:{d}\n", .{port});

while (true) {
    const conn = try server.accept();
    defer conn.stream.close();

    var req_buf: [4096]u8 = undefined;
    const n = try conn.stream.read(&req_buf);
    if (n == 0) continue;

    const resp =
        "HTTP/1.1 200 OK\r\n" ++
        "Content-Type: application/json\r\n" ++
        "Connection: close\r\n\r\n" ++
        "{\"Browser\":\"panda-browser/1.0 (Zig+C++20)\",\"Protocol-Version\":\"1.3\"}\n";
    _ = try conn.stream.writeAll(resp);
}


}