// SPDX-License-Identifier: Apache-2.0
// Loom Path Resolution
//
// Shared by loomc and loomx. Resolves LOOM_HOME and derives all
// tool / data paths from it.
//
// Resolution order:
//   1. LOOM_HOME environment variable (explicit override)
//   2. Build-tree heuristic: <exe_dir>/../build/passes exists
//   3. Install-tree default: <exe_dir>/..

#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace loom {

struct LoomPaths {
    fs::path root;          // LOOM_HOME
    fs::path yosys_bin;     // Yosys binary
    fs::path plugin_dir;    // Directory with .so plugins (install tree)
    fs::path rtl_dir;       // Infrastructure RTL
    fs::path bfm_dir;       // BFM sources
    fs::path sim_top;       // Generic sim testbench
    fs::path include_dir;   // Include dir (for dispatch compilation)
    bool     is_build_tree; // True if running from build tree

    // Plugin paths (may be scattered in build tree)
    fs::path slang_plugin;
    fs::path reset_extract_plugin;
    fs::path scan_insert_plugin;
    fs::path loom_instrument_plugin;
    fs::path emu_top_plugin;
    fs::path mem_shadow_plugin;

    // Get the directory containing the running executable
    static fs::path exe_dir() {
        // /proc/self/exe on Linux, _NSGetExecutablePath on macOS
        // Use std::filesystem::canonical on /proc/self/exe or argv[0]
#if defined(__APPLE__)
        // macOS: use _NSGetExecutablePath
        char buf[4096];
        uint32_t size = sizeof(buf);
        if (::_NSGetExecutablePath(buf, &size) == 0) {
            return fs::canonical(fs::path(buf)).parent_path();
        }
#elif defined(__linux__)
        auto p = fs::read_symlink("/proc/self/exe");
        return p.parent_path();
#endif
        // Fallback: assume current directory
        return fs::current_path();
    }

    static LoomPaths resolve() {
        LoomPaths paths;
        fs::path root;

        // 1. LOOM_HOME environment variable
        if (const char *env = std::getenv("LOOM_HOME")) {
            root = fs::path(env);
            if (!fs::exists(root)) {
                throw std::runtime_error(
                    "LOOM_HOME points to non-existent directory: " +
                    root.string());
            }
        } else {
            // 2/3. Derive from executable location
            root = exe_dir().parent_path(); // exe in bin/ or build/src/tools/
        }

        paths.root = fs::canonical(root);

        // Detect build tree vs install tree
        // Build tree: <root>/build/passes/ exists
        fs::path build_passes = paths.root / "build" / "passes";
        paths.is_build_tree = fs::is_directory(build_passes);

        if (paths.is_build_tree) {
            // Build tree layout
            paths.yosys_bin = paths.root / "build" / "yosys" / "bin" / "yosys";
            paths.slang_plugin =
                paths.root / "build" / "yosys-slang" / "slang.so";
            paths.reset_extract_plugin =
                paths.root / "build" / "passes" / "reset_extract" /
                "reset_extract.so";
            paths.scan_insert_plugin =
                paths.root / "build" / "passes" / "scan_insert" /
                "scan_insert.so";
            paths.loom_instrument_plugin =
                paths.root / "build" / "passes" / "loom_instrument" /
                "loom_instrument.so";
            paths.emu_top_plugin =
                paths.root / "build" / "passes" / "emu_top" / "emu_top.so";
            paths.mem_shadow_plugin =
                paths.root / "build" / "passes" / "mem_shadow" /
                "mem_shadow.so";
            paths.rtl_dir = paths.root / "src" / "rtl";
            paths.bfm_dir = paths.root / "src" / "bfm";
            paths.sim_top = paths.root / "src" / "rtl" / "loom_shell.sv";
            paths.include_dir = paths.root / "src" / "dpi";
        } else {
            // Install tree layout
            paths.yosys_bin = paths.root / "bin" / "yosys";
            paths.plugin_dir = paths.root / "lib" / "loom";
            paths.slang_plugin = paths.plugin_dir / "slang.so";
            paths.reset_extract_plugin = paths.plugin_dir / "reset_extract.so";
            paths.scan_insert_plugin = paths.plugin_dir / "scan_insert.so";
            paths.loom_instrument_plugin =
                paths.plugin_dir / "loom_instrument.so";
            paths.emu_top_plugin = paths.plugin_dir / "emu_top.so";
            paths.mem_shadow_plugin = paths.plugin_dir / "mem_shadow.so";
            paths.rtl_dir = paths.root / "share" / "loom" / "rtl";
            paths.bfm_dir = paths.root / "share" / "loom" / "bfm";
            paths.sim_top =
                paths.root / "share" / "loom" / "rtl" / "loom_shell.sv";
            paths.include_dir = paths.root / "include" / "loom";
        }

        return paths;
    }

    // Return Yosys plugin load arguments: -m plugin1.so -m plugin2.so ...
    std::vector<std::string> plugin_args(bool mem_shadow = false) const {
        std::vector<std::string> args;
        args.push_back("-m");
        args.push_back(slang_plugin.string());
        args.push_back("-m");
        args.push_back(reset_extract_plugin.string());
        args.push_back("-m");
        args.push_back(scan_insert_plugin.string());
        args.push_back("-m");
        args.push_back(loom_instrument_plugin.string());
        args.push_back("-m");
        args.push_back(emu_top_plugin.string());
        if (mem_shadow) {
            args.push_back("-m");
            args.push_back(mem_shadow_plugin.string());
        }
        return args;
    }
};

} // namespace loom
