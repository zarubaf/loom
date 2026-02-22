// SPDX-License-Identifier: Apache-2.0
// loomc — Loom Compilation Driver
//
// Replaces bin/loom bash script.  Runs the Yosys transformation pipeline
// (read_slang → loom_instrument → scan_insert → emu_top → write_verilog)
// then compiles the generated dispatch table into a shared object.
//
// Usage:
//   loomc [options] <sources...>
//   loomc -top my_dut -work build/ my_dut.sv
//   loomc -top my_dut -f dut.f -work build/

#include "loom_paths.h"
#include "loom_log.h"
#include "sha256.h"
#include "toml_utils.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

loom::Logger logger = loom::make_logger("loomc");

struct Options {
    std::string top_module;
    fs::path work_dir = "work";
    std::string clk;          // empty = auto-detect from tbx clkgen, fallback clk_i
    std::string rst = "rst_ni";
    std::vector<fs::path> sources;
    std::vector<fs::path> filelists;
    std::vector<std::string> defines;
    bool verbose = false;
};

void print_usage(const char *prog) {
    std::printf(
        "Usage: %s [options] <sources...>\n"
        "\n"
        "Options:\n"
        "  -top MODULE    Top module name (required)\n"
        "  -work DIR      Work/output directory (default: work/)\n"
        "  -f FILELIST    Read source files from filelist\n"
        "  -clk SIGNAL    Clock signal name (default: clk_i)\n"
        "  -rst SIGNAL    Reset signal name (default: rst_ni)\n"
        "  -D DEFINE      Preprocessor define (passed to slang)\n"
        "  -v             Verbose output\n"
        "  -h             Show this help\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    Options opts;
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "-top" && i + 1 < argc) {
            opts.top_module = argv[++i];
        } else if (arg == "-work" && i + 1 < argc) {
            opts.work_dir = argv[++i];
        } else if (arg == "-f" && i + 1 < argc) {
            opts.filelists.emplace_back(argv[++i]);
        } else if (arg == "-clk" && i + 1 < argc) {
            opts.clk = argv[++i];
        } else if (arg == "-rst" && i + 1 < argc) {
            opts.rst = argv[++i];
        } else if (arg == "-D" && i + 1 < argc) {
            opts.defines.emplace_back(argv[++i]);
        } else if (arg == "-v") {
            opts.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg[0] == '-') {
            logger.error("Unknown option: %s", arg.c_str());
            print_usage(argv[0]);
            std::exit(1);
        } else {
            opts.sources.emplace_back(arg);
        }
        ++i;
    }
    if (opts.top_module.empty()) {
        logger.error("-top is required");
        print_usage(argv[0]);
        std::exit(1);
    }
    if (opts.sources.empty() && opts.filelists.empty()) {
        logger.error("No source files specified");
        print_usage(argv[0]);
        std::exit(1);
    }
    return opts;
}

// Run a subprocess and return its exit code.
// If cwd is non-empty, the child process chdir's there before exec.
int run(const std::vector<std::string> &args, const std::string &cwd = {}) {
    if (!cwd.empty())
        logger.debug("(in %s)", cwd.c_str());
    {
        std::string cmdline;
        for (auto &a : args) {
            if (!cmdline.empty())
                cmdline += ' ';
            cmdline += a;
        }
        logger.debug("%s", cmdline.c_str());
    }

    // Build argv
    std::vector<const char *> c_args;
    for (auto &a : args)
        c_args.push_back(a.c_str());
    c_args.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        logger.error("fork: %s", strerror(errno));
        return 1;
    }
    if (pid == 0) {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            // In child — can't use logger safely after fork
            std::perror("chdir");
            _exit(127);
        }
        execvp(c_args[0], const_cast<char *const *>(c_args.data()));
        std::perror("execvp");
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

// Build the Yosys script string for the transformation pipeline
std::string build_yosys_script(const Options &opts,
                               const loom::LoomPaths &paths) {
    std::ostringstream ys;

    // Read all sources via a single read_slang call so that
    // cross-file module references resolve correctly.
    // --loom: enables DPI bridging, FSM extraction, and tbx clkgen detection.
    // Note: --ignore-initial is NOT used — loom mode selectively handles
    // initial blocks (tbx clkgen → port promotion, waits → skipped).
    ys << "read_slang --loom";
    for (auto &d : opts.defines)
        ys << " -D " << d;
    for (auto &f : opts.filelists)
        ys << " -F " << fs::absolute(f).string();
    for (auto &s : opts.sources)
        ys << " " << fs::absolute(s).string();
    if (!opts.top_module.empty())
        ys << " --top " << opts.top_module;
    ys << "\n";

    // Elaborate — no optimization until after Loom passes wire DPI/finish
    // to output ports. Without outputs, opt_clean removes all internal logic.
    ys << "hierarchy -check -top " << opts.top_module << "\n";
    ys << "proc\n";

    // Memory shadow (before flatten)
    ys << "memory_collect\n";
    ys << "memory_dff\n";
    ys << "mem_shadow";
    if (!opts.clk.empty())
        ys << " -clk " << opts.clk;
    else
        ys << " -clk clk_i";
    ys << " -map mem_map.pb\n";

    // Flatten
    ys << "flatten\n";

    // Extract reset values, strip async resets, remove reset port
    ys << "reset_extract -rst " << opts.rst << "\n";

    // DPI instrument (creates loom_en, DPI/finish output ports).
    // From here on, DPI args/result and finish are module outputs —
    // opt_clean preserves FFs in their fan-in, removes dead ones.
    ys << "loom_instrument -header_out loom_dpi_dispatch.c\n";

    // Optimize: DPI/finish outputs anchor the live fan-in cone.
    // Dead FFs (unused register file entries, tied-off subsystems)
    // are correctly removed before scan chain insertion.
    // WORKAROUND: use opt_expr + opt_merge + opt_clean instead of full opt.
    // Yosys opt_dff treats $memrd output from X-initialized memory as
    // "don't care", proving D==Q for FFs whose data path goes through
    // memory reads (register file, PC). This is incorrect — memory content
    // is unknown at synthesis time. TODO: fix in Yosys opt_dff.
    ys << "opt_expr\n";
    ys << "opt_merge\n";
    ys << "opt_clean\n";

    // Scan insert (after opt — only live FFs end up on the chain)
    ys << "scan_insert -map scan_map.pb\n";


    // Emulation top wrapper
    ys << "emu_top -top " << opts.top_module;
    if (!opts.clk.empty())
    ys << " -clk " << opts.clk;
ys << " -rst " << opts.rst << "\n";

    // Final cleanup
    ys << "opt\n";
    ys << "bwmuxmap\n";
    ys << "write_verilog -noattr transformed.v\n";

    return ys.str();
}

} // namespace

int main(int argc, char **argv) {
    auto opts = parse_args(argc, argv);

    if (opts.verbose) {
        loom::set_log_level(loom::LogLevel::Debug);
    }

    // Resolve LOOM_HOME
    loom::LoomPaths paths;
    try {
        paths = loom::LoomPaths::resolve();
    } catch (const std::exception &e) {
        logger.error("%s", e.what());
        return 1;
    }

    logger.debug("LOOM_HOME: %s", paths.root.c_str());
    logger.debug("Build tree: %s", paths.is_build_tree ? "yes" : "no");

    // Create work directory
    fs::create_directories(opts.work_dir);
    auto work = fs::absolute(opts.work_dir);

    // Write Yosys script to work dir
    auto script = build_yosys_script(opts, paths);
    auto script_path = work / "run.ys";
    {
        std::ofstream f(script_path);
        if (!f) {
            logger.error("Cannot write %s", script_path.c_str());
            return 1;
        }
        f << script;
    }

    logger.debug("Yosys script:\n%s", script.c_str());

    // Step 1: Run Yosys
    logger.info("Running Yosys transformation...");
    {
        auto args = std::vector<std::string>{paths.yosys_bin.string()};
        auto plugins = paths.plugin_args();
        args.insert(args.end(), plugins.begin(), plugins.end());
        args.push_back("-s");
        args.push_back(script_path.string());

        // Run Yosys with CWD = work directory so relative output paths
        // (transformed.v, scan_map.pb, etc.) land in the right place.
        int rc = run(args, work.string());
        if (rc != 0) {
            logger.error("Yosys failed (exit %d)", rc);
            return rc;
        }
    }

    // Verify outputs exist
    auto transformed = work / "transformed.v";
    auto dispatch_c = work / "loom_dpi_dispatch.c";
    if (!fs::exists(transformed)) {
        logger.error("Yosys did not produce %s", transformed.c_str());
        return 1;
    }
    if (!fs::exists(dispatch_c)) {
        logger.error("Yosys did not produce %s", dispatch_c.c_str());
        return 1;
    }

    // Append build metadata to loom_manifest.toml (written by emu_top pass)
    {
        auto manifest_path = work / "loom_manifest.toml";
        if (fs::exists(manifest_path)) {
            // Compute SHA-256 of transformed.v
            std::ifstream tf(transformed, std::ios::binary);
            std::string tv_content((std::istreambuf_iterator<char>(tf)),
                                    std::istreambuf_iterator<char>());
            auto tv_hash = loom::sha256(tv_content);
            auto tv_hex = loom::sha256_hex(tv_hash);

            // Get current UTC timestamp
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm utc;
            gmtime_r(&time_t_now, &utc);
            char ts_buf[64];
            std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", &utc);

            loom::TomlData build_data;
            build_data["build"]["transformed_sha256"] = tv_hex;
            build_data["build"]["timestamp"] = std::string(ts_buf);

            loom::toml_append(manifest_path.string(), build_data);
            logger.info("  loom_manifest.toml (appended build metadata)");
        }
    }

    // Step 2: Compile dispatch table into shared object
    logger.info("Compiling dispatch shared object...");
    auto dispatch_so = work / "loom_dpi_dispatch.so";
    {
        // Use cc (from environment or default)
        std::string cc = "cc";
        if (const char *env_cc = std::getenv("CC"))
            cc = env_cc;

        // Public headers (svdpi.h) in src/include, internal DPI
        // code (loom_svdpi_array.h, svdpi_openarray.c, loom_dpi_service.h)
        // in src/dpi.
        auto svdpi_include = paths.root / "src" / "include";
        auto dpi_dir = paths.root / "src" / "dpi";
        if (!paths.is_build_tree) {
            svdpi_include = paths.root / "include" / "loom";
            dpi_dir = paths.root / "lib" / "loom" / "dpi";
        }

        auto args = std::vector<std::string>{
            cc,
            "-shared",
            "-fPIC",
            "-g",
            "-O0",
            "-I" + paths.include_dir.string(),
            "-I" + svdpi_include.string(),
            "-I" + dpi_dir.string(),
#if defined(__APPLE__)
            // Allow unresolved extern refs to user DPI functions;
            // resolved at runtime when loomx loads the user .so first.
            "-undefined",
            "dynamic_lookup",
#endif
            dispatch_c.string(),
            (dpi_dir / "svdpi_openarray.c").string(),
            "-o",
            dispatch_so.string(),
        };

        int rc = run(args);
        if (rc != 0) {
            logger.error("Dispatch compilation failed (exit %d)", rc);
            return rc;
        }
    }

    logger.info("Done. Work directory: %s", work.c_str());
    logger.info("  transformed.v");
    logger.info("  loom_dpi_dispatch.so");
    logger.info("  scan_map.pb");
    logger.info("  mem_map.pb");
    logger.info("  loom_manifest.toml");

    return 0;
}
