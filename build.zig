const std = @import("std");

pub fn build(b: *std.Build) void {
    // Project C++ build is orchestrated by this pinned Zig entry; the script
    // invokes zig c++ and explicit archives, never host same-name libraries.
    const compile = b.addSystemCommand(&.{ "bash", "tools/build_project.sh", "build" });
    b.getInstallStep().dependOn(&compile.step);
    const run_cmd = b.addSystemCommand(&.{ "bash", "tools/build_project.sh", "run" });
    // Zig 0.17 removed Build.args; direct executable accepts smoke/test flags.
    b.step("run", "Build and run the desktop application").dependOn(&run_cmd.step);
    const dist = b.addSystemCommand(&.{ "bash", "tools/build_project.sh", "dist" });
    b.step("dist-linux-x86_64", "Build stripped Linux development prototype").dependOn(&dist.step);
    const windows = b.addSystemCommand(&.{ "bash", "tools/build_project.sh", "windows" });
    b.step("windows", "Cross-build the Windows x86_64 GUI desktop prototype").dependOn(&windows.step);
    const check = b.addSystemCommand(&.{ "bash", "tools/build_project.sh", "deps-check" });
    b.step("deps-check", "Verify explicit desktop dependency archives").dependOn(&check.step);
}
