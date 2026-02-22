// SPDX-License-Identifier: Apache-2.0
// loomx — Loom Execution Host
//
// Replaces per-test host binary builds.  Loads the dispatch shared object
// (produced by loomc) and an optional user DPI shared object via dlopen,
// launches the Verilator simulation, and runs the Loom shell.
//
// Usage:
//   loomx -work build/                                  # no user DPI
//   loomx -work build/ -sv_lib dpi -sim Vloom_shell   # with user DPI

#include "loom_paths.h"

#include "loom.h"
#include "loom_dpi_service.h"
#include "loom_log.h"
#include "loom_shell.h"
#include "toml_utils.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

loom::Logger logger = loom::make_logger("loomx");

struct Options {
    fs::path work_dir;
    std::string sv_lib;         // User DPI lib name (without lib prefix / .so)
    std::string sim_name = "Vloom_shell";
    std::string script_file;
    std::string socket_path;    // Empty = auto PID-based
    std::string timeout;        // Sim timeout in ns (empty = sim default, "-1" = infinite)
    std::string transport = "socket";  // "socket" or "xdma"
    std::string device;         // XDMA device path (default /dev/xdma0_user)
    std::string dpi_mode = "polling"; // "polling" or "interrupt"
    bool verbose = false;
    bool no_sim = false;
    bool sim_explicit = false;  // true if user passed -sim
};

void print_usage(const char *prog) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -work DIR       Work directory from loomc (required)\n"
        "  -sv_lib NAME    User DPI shared library (without lib/.so)\n"
        "  -sim BINARY     Simulation binary name (default: Vloom_shell)\n"
        "  -f SCRIPT       Run commands from script file\n"
        "  -s SOCKET       Socket path (default: auto PID-based)\n"
        "  -timeout NS     Simulation timeout in ns (-1 for infinite)\n"
        "  -t TRANSPORT    Transport: socket (default) or xdma\n"
        "  -d DEVICE       XDMA device path or PCI BDF (default: /dev/xdma0_user)\n"
        "  -dpi-mode MODE  DPI service mode: polling (default) or interrupt\n"
        "  --no-sim        Don't launch sim (connect to existing socket)\n"
        "  -v              Verbose output\n"
        "  -h              Show this help\n",
        prog);
}

Options parse_args(int argc, char **argv) {
    Options opts;
    int i = 1;
    while (i < argc) {
        std::string arg = argv[i];
        if (arg == "-work" && i + 1 < argc) {
            opts.work_dir = argv[++i];
        } else if (arg == "-sv_lib" && i + 1 < argc) {
            opts.sv_lib = argv[++i];
        } else if (arg == "-sim" && i + 1 < argc) {
            opts.sim_name = argv[++i];
            opts.sim_explicit = true;
        } else if (arg == "-f" && i + 1 < argc) {
            opts.script_file = argv[++i];
        } else if (arg == "-s" && i + 1 < argc) {
            opts.socket_path = argv[++i];
        } else if (arg == "-timeout" && i + 1 < argc) {
            opts.timeout = argv[++i];
        } else if (arg == "-t" && i + 1 < argc) {
            opts.transport = argv[++i];
        } else if (arg == "-d" && i + 1 < argc) {
            opts.device = argv[++i];
        } else if (arg == "-dpi-mode" && i + 1 < argc) {
            opts.dpi_mode = argv[++i];
        } else if (arg == "--no-sim") {
            opts.no_sim = true;
        } else if (arg == "-v") {
            opts.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            logger.error("Unknown option: %s", arg.c_str());
            print_usage(argv[0]);
            std::exit(1);
        }
        ++i;
    }
    if (opts.work_dir.empty()) {
        logger.error("-work is required");
        print_usage(argv[0]);
        std::exit(1);
    }
    if (opts.transport != "socket" && opts.transport != "xdma") {
        logger.error("Unknown transport: %s (expected 'socket' or 'xdma')",
                     opts.transport.c_str());
        std::exit(1);
    }
    if (opts.dpi_mode != "polling" && opts.dpi_mode != "interrupt") {
        logger.error("Unknown DPI mode: %s (expected 'polling' or 'interrupt')",
                     opts.dpi_mode.c_str());
        std::exit(1);
    }
    return opts;
}

// Poll for socket file to appear (sim needs time to start)
bool wait_for_socket(const std::string &path, int timeout_ms) {
    int elapsed = 0;
    constexpr int poll_ms = 100;
    while (elapsed < timeout_ms) {
        if (fs::exists(path))
            return true;
        usleep(poll_ms * 1000);
        elapsed += poll_ms;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    auto opts = parse_args(argc, argv);

    // Ignore SIGPIPE — the simulation may close the socket before we
    // finish reading/writing, which is normal termination.
    signal(SIGPIPE, SIG_IGN);

    if (opts.verbose) {
        loom::set_log_level(loom::LogLevel::Debug);
    }

    auto work = fs::absolute(opts.work_dir);
    if (!fs::is_directory(work)) {
        logger.error("Work directory not found: %s", work.c_str());
        return 1;
    }

    // XDMA transport: validate flags
    bool use_xdma = (opts.transport == "xdma");
    if (use_xdma) {
        if (opts.sim_explicit) {
            logger.error("Cannot launch simulation with XDMA transport");
            return 1;
        }
        opts.no_sim = true;
        if (opts.device.empty()) {
            opts.device = "/dev/xdma0_user";
        }
    }

    // Auto socket path: PID-based for parallel safety (socket mode only)
    if (!use_xdma && opts.socket_path.empty()) {
        opts.socket_path =
            "/tmp/loom_sim_" + std::to_string(getpid()) + ".sock";
    }

    // ========================================================================
    // DPI loading (two-stage dlopen)
    // ========================================================================

    // Initialize to null (for designs without DPI)
    const loom_dpi_func_t *funcs = nullptr;
    const int *n_funcs = nullptr;
    void *dispatch_handle = nullptr;
    void *user_handle = nullptr;

    // Only load DPI libraries if dispatch library exists
    auto dispatch_path = work / "loom_dpi_dispatch.so";
    if (fs::exists(dispatch_path)) {
        // Stage 1: Load dispatch .so first — it provides svdpi open array
        // functions (svGetArrayPtr etc.) that the user library may depend on.
        logger.info("Loading dispatch library: %s", dispatch_path.c_str());
        dispatch_handle = dlopen(dispatch_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        if (!dispatch_handle) {
            logger.error("Failed to load dispatch library: %s", dlerror());
            return 1;
        }

        // Stage 2: Load user .so with RTLD_GLOBAL (exports user function symbols
        // that the dispatch wrappers call via -undefined dynamic_lookup)
        if (!opts.sv_lib.empty()) {
            // Resolve library path (SystemVerilog -sv_lib convention):
            //   -sv_lib foo       → try foo.so, then libfoo.so
            //   -sv_lib dir/foo   → try dir/foo.so, then dir/libfoo.so
            std::string user_lib;
            fs::path sv_path(opts.sv_lib);
            fs::path with_so = sv_path;
            with_so += ".so";
            fs::path with_lib = sv_path.parent_path() / ("lib" + sv_path.filename().string() + ".so");

            if (fs::exists(with_so)) {
                user_lib = fs::absolute(with_so).string();
            } else if (fs::exists(with_lib)) {
                user_lib = fs::absolute(with_lib).string();
            } else {
                // Try as a direct path
                user_lib = opts.sv_lib;
            }

            logger.info("Loading user DPI library: %s", user_lib.c_str());
            user_handle = dlopen(user_lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
            if (!user_handle) {
                logger.error("Failed to load user library: %s", dlerror());
                return 1;
            }
        }

        // Get function table from dispatch .so
        funcs = reinterpret_cast<const loom_dpi_func_t *>(
            dlsym(dispatch_handle, "loom_dpi_funcs"));
        n_funcs =
            reinterpret_cast<const int *>(dlsym(dispatch_handle, "loom_dpi_n_funcs"));

        if (!funcs || !n_funcs) {
            logger.error("Dispatch library missing loom_dpi_funcs/loom_dpi_n_funcs");
            return 1;
        }

        logger.info("Loaded %d DPI functions from dispatch table", *n_funcs);
    } else {
        logger.info("No dispatch library found - design has no DPI calls");
    }

    // ========================================================================
    // Launch simulation (unless --no-sim)
    // ========================================================================

    pid_t sim_pid = -1;
    if (!opts.no_sim) {
        // Find simulation binary
        auto sim_bin =
            work / "sim" / "obj_dir" / opts.sim_name;
        if (!fs::exists(sim_bin)) {
            logger.error("Simulation binary not found: %s",
                         sim_bin.c_str());
            return 1;
        }

        // Clean up any stale socket
        if (fs::exists(opts.socket_path)) {
            fs::remove(opts.socket_path);
        }

        logger.info("Launching simulation: %s", sim_bin.c_str());
        logger.info("Socket: %s", opts.socket_path.c_str());

        sim_pid = fork();
        if (sim_pid < 0) {
            logger.error("fork: %s", strerror(errno));
            return 1;
        }
        if (sim_pid == 0) {
            // Child: exec simulation with plusargs
            std::vector<std::string> sim_args = {
                sim_bin, "+socket=" + opts.socket_path,
                "+verilator+rand+reset+2"
            };
            if (!opts.timeout.empty())
                sim_args.push_back("+timeout=" + opts.timeout);

            std::vector<const char *> argv;
            for (auto &a : sim_args) argv.push_back(a.c_str());
            argv.push_back(nullptr);

            execv(sim_bin.c_str(), const_cast<char *const *>(argv.data()));
            // In child — can't use logger safely after fork
            std::perror("execv");
            _exit(127);
        }

        // Parent: wait for socket to appear
        if (!wait_for_socket(opts.socket_path, 10000)) {
            logger.error("Timeout waiting for simulation socket");
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, nullptr, 0);
            return 1;
        }
    }

    // ========================================================================
    // Connect and run shell
    // ========================================================================

    std::unique_ptr<loom::Transport> transport;
    std::string connect_target;

    if (use_xdma) {
        transport = loom::create_xdma_transport();
        connect_target = opts.device;
    } else {
        transport = loom::create_socket_transport();
        connect_target = opts.socket_path;
    }

    if (!transport) {
        logger.error("Failed to create transport");
        if (sim_pid > 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, nullptr, 0);
        }
        return 1;
    }

    loom::Context ctx(std::move(transport));

    logger.info("Connecting to %s...", connect_target.c_str());
    auto rc = ctx.connect(connect_target);
    if (!rc.ok()) {
        logger.error("Failed to connect to %s", connect_target.c_str());
        if (sim_pid > 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, nullptr, 0);
        }
        return 1;
    }

    // Read manifest and verify design hash + shell version
    {
        auto manifest_path = work / "loom_manifest.toml";
        if (fs::exists(manifest_path)) {
            auto manifest = loom::toml_read(manifest_path.string());

            // Compare design hash
            auto it_design = manifest.find("design");
            if (it_design != manifest.end()) {
                auto it_hash = it_design->second.find("hash");
                if (it_hash != it_design->second.end()) {
                    std::string manifest_hash = it_hash->second;
                    std::string hw_hash = ctx.design_hash_hex();
                    if (manifest_hash != hw_hash) {
                        logger.warning("Design hash mismatch!");
                        logger.warning("  Manifest: %s", manifest_hash.c_str());
                        logger.warning("  Hardware: %s", hw_hash.c_str());
                        logger.warning("  The hardware may have been built from a different design.");
                    }
                }
            }

            // Compare shell version
            auto it_shell = manifest.find("shell");
            if (it_shell != manifest.end()) {
                auto it_ver = it_shell->second.find("version_hex");
                if (it_ver != it_shell->second.end()) {
                    uint32_t manifest_ver = static_cast<uint32_t>(
                        std::strtoul(it_ver->second.c_str(), nullptr, 0));
                    uint32_t hw_ver = ctx.shell_version();

                    uint32_t sw_major = (manifest_ver >> 16) & 0xFF;
                    uint32_t hw_major = (hw_ver >> 16) & 0xFF;
                    uint32_t sw_minor = (manifest_ver >> 8) & 0xFF;
                    uint32_t hw_minor = (hw_ver >> 8) & 0xFF;

                    if (sw_major != hw_major) {
                        logger.warning("Shell major version mismatch! SW=%s HW=%s",
                                       loom::version_string(manifest_ver).c_str(),
                                       loom::version_string(hw_ver).c_str());
                    } else if (hw_minor > sw_minor) {
                        logger.warning("Shell is newer than loomx (HW=%s SW=%s)",
                                       loom::version_string(hw_ver).c_str(),
                                       loom::version_string(manifest_ver).c_str());
                    }
                }
            }
        } else {
            logger.debug("No loom_manifest.toml found in work directory");
        }
    }

    // Configure DPI service
    auto &dpi_service = loom::global_dpi_service();
    dpi_service.set_mode(opts.dpi_mode == "interrupt" ? loom::DpiMode::Interrupt
                                                      : loom::DpiMode::Polling);
    if (funcs && n_funcs) {
        if (ctx.n_dpi_funcs() > static_cast<uint32_t>(*n_funcs)) {
            logger.warning("Design has %u DPI funcs but dispatch only has %d",
                           ctx.n_dpi_funcs(), *n_funcs);
        }
        dpi_service.register_funcs(funcs, *n_funcs);
    } else {
        // No DPI functions in design
        if (ctx.n_dpi_funcs() > 0) {
            logger.warning("Design has %u DPI funcs but no dispatch library loaded",
                           ctx.n_dpi_funcs());
        }
    }

    // Run shell
    loom::Shell shell(ctx, dpi_service);

    // Load scan map for named variable dump/inspect
    auto scan_map_path = work / "scan_map.pb";
    shell.load_scan_map(scan_map_path.string());

    // Load memory map for memory preload/dump
    auto mem_map_path = work / "mem_map.pb";
    if (fs::exists(mem_map_path))
        shell.load_mem_map(mem_map_path.string());

    int exit_code;
    if (!opts.script_file.empty()) {
        exit_code = shell.run_script(opts.script_file);
    } else {
        exit_code = shell.run_interactive();
    }

    // Final stats
    auto cycle_result = ctx.get_cycle_count();
    if (cycle_result.ok()) {
        logger.info("Final cycle count: %llu",
                    static_cast<unsigned long long>(cycle_result.value()));
    }
    dpi_service.print_stats();

    // Tell simulation to finish cleanly (allows trace flush)
    if (sim_pid > 0 && ctx.is_connected()) {
        ctx.finish(exit_code);
        // Give sim time to process $finish and flush traces
        usleep(100000);  // 100ms
    }

    // Disconnect
    ctx.disconnect();

    // Clean up simulation process
    if (sim_pid > 0) {
        // Wait briefly for clean exit, then force kill
        int status;
        pid_t rc = waitpid(sim_pid, &status, WNOHANG);
        if (rc == 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, nullptr, 0);
        }
        // Remove socket file
        if (fs::exists(opts.socket_path)) {
            fs::remove(opts.socket_path);
        }
    }

    // Close dlopen handles
    if (dispatch_handle)
        dlclose(dispatch_handle);
    if (user_handle)
        dlclose(user_handle);

    // SIGPIPE (exit 141) is expected when sim terminates before host
    if (exit_code == 141)
        exit_code = 0;

    return exit_code;
}
