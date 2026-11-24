// ============================================================================
// FILE: panda-browser/src/main.zig
// Standalone Zig 0.17+ entry point: GUI window + headless ArenaAllocator CLI
// ============================================================================
const std = @import("std");
const opts = @import("build_options");

comptime {
    if (opts.max_pages == 0) {
        @compileError("max-pages must be greater than 0");
    }
    if (opts.arena_kb == 0) {
        @compileError("arena-kb must be greater than 0");
    }
}

// C-ABI declarations matching src/engine_bridge.cpp
const PandaParseStats = extern struct {
    text_len: usize,
    title_len: usize,
    link_count: usize,
    script_count: usize,
    parse_us: u32,
};

extern fn cpp_engine_init(enable_simd: bool, max_pages: u32) i32;
extern fn cpp_parse_document(
    html_ptr: [*]const u8,
    html_len: usize,
    title_buf: [*]u8,
    title_cap: usize,
    text_buf: [*]u8,
    text_cap: usize,
) PandaParseStats;
extern fn cpp_eval_script(
    script_ptr: [*]const u8,
    script_len: usize,
    out_buf: [*]u8,
    out_cap: usize,
) usize;
extern fn cpp_launch_gui(initial_url: [*:0]const u8) i32;
extern fn cpp_engine_shutdown() void;

fn looksLikeGuiTarget(arg: []const u8) bool {
    if (arg.len == 0 or arg[0] == '<' or arg[0] == '-') {
        return false;
    }
    if (arg[0] == '/' or
        std.mem.startsWith(u8, arg, "http://") or
        std.mem.startsWith(u8, arg, "https://") or
        std.mem.startsWith(u8, arg, "file://") or
        std.mem.startsWith(u8, arg, "about:"))
    {
        return true;
    }
    var has_dot = false;
    for (arg) |ch| {
        if (ch == ' ') {
            return false;
        }
        if (ch == '.') {
            has_dot = true;
        }
    }
    return has_dot or std.mem.startsWith(u8, arg, "localhost");
}

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const base_alloc = init.gpa;
    const args = try init.minimal.args.toSlice(init.arena.allocator());

    if (cpp_engine_init(opts.use_simd, opts.max_pages) != 0) {
        return error.CppEngineInitFailed;
    }
    defer cpp_engine_shutdown();

    // No arguments (or empty %u passed by a desktop launcher):
    // open the full GUI window if enabled, otherwise show build info.
    if (args.len < 2 or (comptime opts.use_gui and args[1].len == 0)) {
        if (comptime opts.use_gui) {
            if (cpp_launch_gui("https://duckduckgo.com") != 0) {
                return error.GuiLaunchFailed;
            }
        } else {
            printBuildInfo(io);
        }
        return;
    }

    if (std.mem.eql(u8, args[1], "--info") or std.mem.eql(u8, args[1], "--help") or std.mem.eql(u8, args[1], "-h")) {
        printBuildInfo(io);
        return;
    }

    if (std.mem.eql(u8, args[1], "--gui")) {
        if (comptime !opts.use_gui) {
            std.debug.print("Error: Recompile with -Duse-gui=true to enable GUI mode.\n", .{});
            return;
        } else {
            const url: [*:0]const u8 = if (args.len >= 3 and args[2].len > 0) args[2].ptr else "https://duckduckgo.com";
            if (cpp_launch_gui(url) != 0) {
                return error.GuiLaunchFailed;
            }
            return;
        }
    }

    if (std.mem.eql(u8, args[1], "--headless")) {
        if (args.len < 3) {
            std.debug.print("Usage: panda-browser --headless <url-or-html>\n", .{});
            return;
        }
        try processTargetPage(io, base_alloc, args[2]);
        return;
    }

    if (std.mem.eql(u8, args[1], "--eval")) {
        if (args.len < 3) {
            std.debug.print("Usage: panda-browser --eval \"<expression>\"\n", .{});
            return;
        }
        try evalInlineScript(io, base_alloc, args[2]);
        return;
    }

    if (std.mem.eql(u8, args[1], "--cdp")) {
        if (comptime !opts.use_cdp) {
            std.debug.print("Error: Recompile with -Duse-cdp=true to enable CDP server.\n", .{});
            return;
        } else {
            const port: u16 = if (args.len >= 3) try std.fmt.parseInt(u16, args[2], 10) else 9222;
            try runCdpServer(io, port);
            return;
        }
    }

    if (std.mem.eql(u8, args[1], "--file")) {
        if (args.len < 3) {
            std.debug.print("Usage: panda-browser --file <path.html>\n", .{});
            return;
        }
        try processFileTarget(io, base_alloc, args[2]);
        return;
    }

    // If GUI is enabled and user/launcher passed a URL, domain, or file path,
    // open it directly in the browser window.
    if (comptime opts.use_gui) {
        if (looksLikeGuiTarget(args[1])) {
            if (cpp_launch_gui(args[1].ptr) != 0) {
                return error.GuiLaunchFailed;
            }
            return;
        }
    }

    try processTargetPage(io, base_alloc, args[1]);
}

fn printBuildInfo(io: std.Io) void {
    var buf: [2048]u8 = undefined;
    var stdout_writer = std.Io.File.stdout().writerStreaming(io, &buf);
    const out = &stdout_writer.interface;

    out.print(
        \\panda-browser (Zig + C++20 Hybrid Browser Engine)
        \\Active Gentoo-Style USE Flags:
        \\  use-gui     : {}
        \\  use-quickjs : {} (headless --eval tag; GUI uses WebKit JavaScriptCore)
        \\  use-v8      : {} (headless --eval tag; GUI uses WebKit JavaScriptCore)
        \\  use-simd    : {}
        \\  use-cdp     : {}
        \\  use-net     : {}
        \\  max-pages   : {d}
        \\  arena-kb    : {d} KB
        \\
        \\Usage:
        \\  panda-browser                      (launch GUI browser when use-gui=true)
        \\  panda-browser https://example.com  (open URL in GUI, or headless if use-gui=false)
        \\  panda-browser --gui [url]          (force GUI window)
        \\  panda-browser --headless <target>  (force fast headless text/link dump)
        \\  panda-browser --file page.html     (parse local HTML file headlessly)
        \\  panda-browser --eval "1 + 1"
        \\  panda-browser --cdp 9222
        \\
    , .{
        opts.use_gui,
        opts.use_quickjs,
        opts.use_v8,
        opts.use_simd,
        opts.use_cdp,
        opts.use_net,
        opts.max_pages,
        opts.arena_kb,
    }) catch {};
    out.flush() catch {};
}

fn evalInlineScript(io: std.Io, alloc: std.mem.Allocator, script_src: []const u8) !void {
    const out_buf = try alloc.alloc(u8, 4096);
    defer alloc.free(out_buf);

    const written = cpp_eval_script(
        script_src.ptr,
        script_src.len,
        out_buf.ptr,
        out_buf.len,
    );

    var io_buf: [1024]u8 = undefined;
    var stdout_writer = std.Io.File.stdout().writerStreaming(io, &io_buf);
    const out = &stdout_writer.interface;
    try out.print("{s}\n", .{out_buf[0..written]});
    try out.flush();
}

fn processFileTarget(io: std.Io, base_alloc: std.mem.Allocator, file_path: []const u8) !void {
    var arena = std.heap.ArenaAllocator.init(base_alloc);
    defer arena.deinit();
    const alloc = arena.allocator();

    const max_bytes: usize = @as(usize, opts.arena_kb) * 1024 * 16; // Hard safety cap (16x arena_kb)
    const html_bytes = try std.Io.Dir.cwd().readFileAlloc(io, file_path, alloc, .limited(max_bytes));
    try parseAndPrint(io, alloc, html_bytes);
}

fn processTargetPage(io: std.Io, base_alloc: std.mem.Allocator, target: []const u8) !void {
    // Per-page ArenaAllocator: everything allocated for this page is freed at once
    var arena = std.heap.ArenaAllocator.init(base_alloc);
    defer arena.deinit();
    const alloc = arena.allocator();

    var html_bytes: []const u8 = target;

    if (std.mem.startsWith(u8, target, "http://") or std.mem.startsWith(u8, target, "https://")) {
        if (comptime !opts.use_net) {
            std.debug.print("Error: Recompile with -Duse-net=true to fetch URLs.\n", .{});
            return;
        } else {
            var client: std.http.Client = .{
                .allocator = alloc,
                .io = io,
            };
            defer client.deinit();

            var body_writer: std.Io.Writer.Allocating = .init(alloc);
            defer body_writer.deinit();

            const extra_headers = [_]std.http.Header{
                .{ .name = "User-Agent", .value = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15" },
                .{ .name = "Accept", .value = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8" },
                .{ .name = "Accept-Language", .value = "en-US,en;q=0.9" },
            };

            const res = try client.fetch(.{
                .location = .{ .url = target },
                .extra_headers = &extra_headers,
                .response_writer = &body_writer.writer,
            });
            if (res.status != .ok) {
                std.debug.print("HTTP Status: {d}\n", .{@intFromEnum(res.status)});
            }
            html_bytes = body_writer.written();
        }
    }

    try parseAndPrint(io, alloc, html_bytes);
}

fn parseAndPrint(io: std.Io, alloc: std.mem.Allocator, html_bytes: []const u8) !void {
    // Extracted text is never larger than raw HTML + 1 NUL byte; avoid over-allocating 512 KB for small pages.
    const title_cap: usize = @max(@as(usize, 1), @min(@as(usize, 1024), html_bytes.len + 1));
    const text_cap: usize = html_bytes.len + 1;

    const title_buf = try alloc.alloc(u8, title_cap);
    const text_buf = try alloc.alloc(u8, text_cap);

    const stats = cpp_parse_document(
        html_bytes.ptr,
        html_bytes.len,
        title_buf.ptr,
        title_buf.len,
        text_buf.ptr,
        text_buf.len,
    );

    var io_buf: [2048]u8 = undefined;
    var stdout_writer = std.Io.File.stdout().writerStreaming(io, &io_buf);
    const out = &stdout_writer.interface;
    try out.print(
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
    try out.flush();
}

fn runCdpServer(io: std.Io, port: u16) !void {
    const addr = try std.Io.net.IpAddress.parseIp4("127.0.0.1", port);
    var server = try addr.listen(io, .{ .reuse_address = true });
    defer server.deinit(io);

    std.debug.print("panda-browser CDP JSON-RPC endpoint listening on 127.0.0.1:{d}\n", .{port});

    while (true) {
        const stream = try server.accept(io);
        defer stream.close(io);

        var req_buf: [4096]u8 = undefined;
        var reader_wrapper = stream.reader(io, &req_buf);
        const reader = &reader_wrapper.interface;

        reader.fillMore() catch |err| switch (err) {
            error.EndOfStream => continue,
            else => return err,
        };
        const req = reader.buffered();
        if (req.len == 0) continue;

        var write_buf: [1024]u8 = undefined;
        var writer_wrapper = stream.writer(io, &write_buf);
        const writer = &writer_wrapper.interface;

        var body_buf: [512]u8 = undefined;
        var resp_buf: [768]u8 = undefined;

        const is_list = std.mem.startsWith(u8, req, "GET /json/list") or
            std.mem.startsWith(u8, req, "GET /json ");
        const is_version = std.mem.startsWith(u8, req, "GET /json/version") or
            std.mem.startsWith(u8, req, "GET / ");

        if (is_list) {
            const body = try std.fmt.bufPrint(
                &body_buf,
                "[{{\"id\":\"page-1\",\"type\":\"page\",\"title\":\"about:blank\",\"url\":\"about:blank\",\"webSocketDebuggerUrl\":\"ws://127.0.0.1:{d}/devtools/page/1\"}}]\n",
                .{port},
            );
            const resp = try std.fmt.bufPrint(
                &resp_buf,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {d}\r\nConnection: close\r\n\r\n{s}",
                .{ body.len, body },
            );
            try writer.writeAll(resp);
        } else if (is_version) {
            const body = try std.fmt.bufPrint(
                &body_buf,
                "{{\"Browser\":\"panda-browser/1.0 (Zig+C++20)\",\"Protocol-Version\":\"1.3\",\"User-Agent\":\"panda-browser/1.0\",\"webSocketDebuggerUrl\":\"ws://127.0.0.1:{d}/devtools/browser/1\"}}\n",
                .{port},
            );
            const resp = try std.fmt.bufPrint(
                &resp_buf,
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {d}\r\nConnection: close\r\n\r\n{s}",
                .{ body.len, body },
            );
            try writer.writeAll(resp);
        } else {
            const body = "{\"error\":\"not found\"}\n";
            const resp = try std.fmt.bufPrint(
                &resp_buf,
                "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: {d}\r\nConnection: close\r\n\r\n{s}",
                .{ body.len, body },
            );
            try writer.writeAll(resp);
        }
        try writer.flush();
    }
}

test "cpp_parse_document extracts title, links, scripts, and text" {
    try std.testing.expectEqual(@as(i32, 0), cpp_engine_init(opts.use_simd, opts.max_pages));
    defer cpp_engine_shutdown();

    const html =
        \\<html><head><title>Panda Test</title>
        \\<style>body { color: red; }</style>
        \\<script>const x = "<a href='fake'>ignored</a>";</script>
        \\</head><body><!-- <a href="comment">hidden</a> --><p>Hello</p><p><a href="/next">World</a></p></body></html>
    ;
    var title_buf: [128]u8 = undefined;
    var text_buf: [256]u8 = undefined;

    const stats = cpp_parse_document(
        html.ptr,
        html.len,
        &title_buf,
        title_buf.len,
        &text_buf,
        text_buf.len,
    );

    try std.testing.expectEqualStrings("Panda Test", title_buf[0..stats.title_len]);
    try std.testing.expectEqualStrings("Hello World", text_buf[0..stats.text_len]);
    try std.testing.expectEqual(@as(usize, 1), stats.link_count);
    try std.testing.expectEqual(@as(usize, 1), stats.script_count);
}

test "cpp_eval_script prefixes active engine tag" {
    const src = "1 + 1";
    var out_buf: [64]u8 = undefined;
    const n = cpp_eval_script(src.ptr, src.len, &out_buf, out_buf.len);
    try std.testing.expect(n > src.len);
    try std.testing.expect(std.mem.endsWith(u8, out_buf[0..n], src));
}
