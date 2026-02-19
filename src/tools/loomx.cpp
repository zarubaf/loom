// SPDX-License-Identifier: Apache-2.0
// loomx — Loom Execution Host
//
// Replaces per-test host binary builds.  Loads the dispatch shared object
// (produced by loomc) and an optional user DPI shared object via dlopen,
// launches the Verilator simulation, and runs the Loom shell.
//
// Usage:
//   loomx -work build/                                  # no user DPI
//   loomx -work build/ -sv_lib dpi -sim Vloom_sim_top   # with user DPI

#include "loom_paths.h"

#include "loom.h"
#include "loom_dpi_service.h"
#include "loom_log.h"
#include "loom_shell.h"

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
    std::string sim_name = "Vloom_sim_top";
    std::string script_file;
    std::string socket_path;    // Empty = auto PID-based
    bool verbose = false;
    bool no_sim = false;
};

void print_usage(const char *prog) {
    std::printf(
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -work DIR       Work directory from loomc (required)\n"
        "  -sv_lib NAME    User DPI shared library (without lib/.so)\n"
        "  -sim BINARY     Simulation binary name (default: Vloom_sim_top)\n"
        "  -f SCRIPT       Run commands from script file\n"
        "  -s SOCKET       Socket path (default: auto PID-based)\n"
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
        } else if (arg == "-f" && i + 1 < argc) {
            opts.script_file = argv[++i];
        } else if (arg == "-s" && i + 1 < argc) {
            opts.socket_path = argv[++i];
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

    // Auto socket path: PID-based for parallel safety
    if (opts.socket_path.empty()) {
        opts.socket_path =
            "/tmp/loom_sim_" + std::to_string(getpid()) + ".sock";
    }

    // ========================================================================
    // DPI loading (two-stage dlopen)
    // ========================================================================

    // Stage 1: Load dispatch .so first — it provides svdpi open array
    // functions (svGetArrayPtr etc.) that the user library may depend on.
    auto dispatch_path = work / "loom_dpi_dispatch.so";
    if (!fs::exists(dispatch_path)) {
        logger.error("Dispatch library not found: %s",
                     dispatch_path.c_str());
        return 1;
    }

    logger.info("Loading dispatch library: %s", dispatch_path.c_str());
    void *dispatch_handle =
        dlopen(dispatch_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if (!dispatch_handle) {
        logger.error("Failed to load dispatch library: %s", dlerror());
        return 1;
    }

    // Stage 2: Load user .so with RTLD_GLOBAL (exports user function symbols
    // that the dispatch wrappers call via -undefined dynamic_lookup)
    void *user_handle = nullptr;
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
    auto *funcs = reinterpret_cast<const loom_dpi_func_t *>(
        dlsym(dispatch_handle, "loom_dpi_funcs"));
    auto *n_funcs =
        reinterpret_cast<const int *>(dlsym(dispatch_handle, "loom_dpi_n_funcs"));

    if (!funcs || !n_funcs) {
        logger.error("Dispatch library missing loom_dpi_funcs/loom_dpi_n_funcs");
        return 1;
    }

    logger.info("Loaded %d DPI functions from dispatch table", *n_funcs);

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
            // Child: exec simulation with +socket= plusarg
            std::string socket_arg = "+socket=" + opts.socket_path;
            execl(sim_bin.c_str(), sim_bin.c_str(), socket_arg.c_str(),
                  nullptr);
            // In child — can't use logger safely after fork
            std::perror("execl");
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

    auto transport = loom::create_socket_transport();
    if (!transport) {
        logger.error("Failed to create transport");
        if (sim_pid > 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, nullptr, 0);
        }
        return 1;
    }

    loom::Context ctx(std::move(transport));

    logger.info("Connecting to %s...", opts.socket_path.c_str());
    auto rc = ctx.connect(opts.socket_path);
    if (!rc.ok()) {
        logger.error("Failed to connect to simulation");
        if (sim_pid > 0) {
            kill(sim_pid, SIGTERM);
            waitpid(sim_pid, nullptr, 0);
        }
        return 1;
    }

    // Verify DPI function count
    if (ctx.n_dpi_funcs() != static_cast<uint32_t>(*n_funcs)) {
        logger.warning("Design has %u DPI funcs, dispatch has %d",
                       ctx.n_dpi_funcs(), *n_funcs);
    }

    // Register DPI functions
    auto &dpi_service = loom::global_dpi_service();
    dpi_service.register_funcs(funcs, *n_funcs);

    // Run shell
    loom::Shell shell(ctx, dpi_service);
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

    // Disconnect
    ctx.disconnect();

    // Clean up simulation process
    if (sim_pid > 0) {
        kill(sim_pid, SIGTERM);
        waitpid(sim_pid, nullptr, 0);
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
