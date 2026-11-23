// ============================================================================
// FILE: panda-browser/build.zig
// Gentoo-style USE flags, C++20 compiler flags, and target wiring (Zig 0.16+)
// ============================================================================
const std = @import("std");

pub fn build(b: *std.Build) void {
    // 1. Gentoo-style target & CFLAGS (-Dcpu=native -Doptimize=ReleaseFast)
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // 2. Gentoo-style USE flags (compile-time feature toggles)
    const use_v8 = b.option(bool, "use-v8", "Link full V8 C++ JIT engine") orelse false;
    const raw_quickjs = b.option(bool, "use-quickjs", "Enable ultra-light QuickJS engine") orelse true;
    // V8 takes precedence if both flags are enabled so Zig options & C++ defines match
    const use_quickjs = raw_quickjs and !use_v8;
    const use_simd = b.option(bool, "use-simd", "Enable C++20 vectorized/SIMD HTML scanner") orelse true;
    const use_cdp = b.option(bool, "use-cdp", "Compile Chrome DevTools Protocol (CDP) server") orelse true;
    const use_net = b.option(bool, "use-net", "Enable native HTTP/HTTPS page fetcher") orelse true;
    const max_pages = b.option(u32, "max-pages", "Compile-time limit on concurrent page arenas") orelse 64;
    const arena_kb = b.option(u32, "arena-kb", "Default per-page arena reserve in KB") orelse 512;

    const options = b.addOptions();
    options.addOption(bool, "use_v8", use_v8);
    options.addOption(bool, "use_quickjs", use_quickjs);
    options.addOption(bool, "use_simd", use_simd);
    options.addOption(bool, "use_cdp", use_cdp);
    options.addOption(bool, "use_net", use_net);
    options.addOption(u32, "max_pages", max_pages);
    options.addOption(u32, "arena_kb", arena_kb);

    // 3. Assemble C++20 flags on a stack buffer
    var flag_buf: [16][]const u8 = undefined;
    var flag_count: usize = 0;

    const base_flags = [_][]const u8{
        "-std=c++20",
        "-Isrc",
        "-fno-exceptions",
        "-fno-rtti",
        "-Wall",
        "-Wextra",
    };
    for (base_flags) |f| {
        flag_buf[flag_count] = f;
        flag_count += 1;
    }

    if (use_simd) {
        flag_buf[flag_count] = "-DUSE_SIMD_PARSER=1";
        flag_count += 1;
    }
    if (use_v8) {
        flag_buf[flag_count] = "-DUSE_V8_ENGINE=1";
        flag_count += 1;
    } else if (use_quickjs) {
        flag_buf[flag_count] = "-DUSE_QUICKJS_ENGINE=1";
        flag_count += 1;
    }
    if (use_cdp) {
        flag_buf[flag_count] = "-DUSE_CDP_BRIDGE=1";
        flag_count += 1;
    }

    const cpp_flags = flag_buf[0..flag_count];

    const opt_name = @tagName(optimize);
    const should_strip = std.mem.eql(u8, opt_name, "ReleaseFast") or
        std.mem.eql(u8, opt_name, "ReleaseSmall") or
        std.mem.eql(u8, opt_name, "fast") or
        std.mem.eql(u8, opt_name, "small") or
        std.mem.eql(u8, opt_name, "release_fast") or
        std.mem.eql(u8, opt_name, "release_small");

    // 4. Root module configuration (Zig 0.16+ Module API)
    const root_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
        .strip = should_strip,
    });
    root_mod.addOptions("build_options", options);
    root_mod.addCSourceFile(.{
        .file = b.path("src/engine_bridge.cpp"),
        .flags = cpp_flags,
    });

    // 5. Executable definition
    const exe = b.addExecutable(.{
        .name = "panda-browser",
        .root_module = root_mod,
    });

    b.installArtifact(exe);

    // `zig build run -- <args>` helper
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run the compiled panda-browser binary");
    run_step.dependOn(&run_cmd.step);

    // `zig build test` helper
    const test_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
        .link_libcpp = true,
    });
    test_mod.addOptions("build_options", options);
    test_mod.addCSourceFile(.{
        .file = b.path("src/engine_bridge.cpp"),
        .flags = cpp_flags,
    });

    const unit_tests = b.addTest(.{
        .root_module = test_mod,
    });
    const run_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run Zig + C++20 bridge unit tests");
    test_step.dependOn(&run_tests.step);
}
