// SPDX-License-Identifier: Apache-2.0
// Loom Simulation Host - Main Entry Point
//
// Provides the main() for Loom simulation hosts. Supports two modes:
//   - Interactive: REPL shell with tab completion, hints, history
//   - Script:      Execute commands from a file (-f flag)
//
// Usage:
//   1. Include the generated loom_dpi_dispatch.c (defines loom_dpi_funcs)
//   2. Link this file with your dpi_impl.c
//
// The user must provide (typically via generated loom_dpi_dispatch.c):
//   extern const loom_dpi_func_t loom_dpi_funcs[];
//   extern const int loom_dpi_n_funcs;

#include "loom.h"
#include "loom_log.h"
#include "loom_dpi_service.h"
#include "loom_shell.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// User-provided DPI function table (from generated loom_dpi_dispatch.c)
extern "C" {
    extern const loom_dpi_func_t loom_dpi_funcs[];
    extern const int loom_dpi_n_funcs;
}

namespace {
    loom::Logger logger = loom::make_logger("main");
}

static void print_usage(const char* prog) {
    std::printf("Usage: %s [options] [socket_path]\n", prog);
    std::printf("Options:\n");
    std::printf("  -f <script>   Execute commands from script file\n");
    std::printf("  -v            Verbose (debug logging)\n");
    std::printf("  -h            Show this help\n");
    std::printf("Default socket: /tmp/loom_sim.sock\n");
}

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/loom_sim.sock";
    std::string script_file;
    bool verbose = false;

    // Parse options
    int opt;
    while ((opt = getopt(argc, argv, "f:vh")) != -1) {
        switch (opt) {
        case 'f':
            script_file = optarg;
            break;
        case 'v':
            verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    // Remaining positional argument is socket path
    if (optind < argc) {
        socket_path = argv[optind];
    }

    if (verbose) {
        loom::set_log_level(loom::LogLevel::Debug);
    }

    logger.info("Loom Simulation Host");
    logger.info("Socket: %s", socket_path.c_str());

    // Create transport and context
    auto transport = loom::create_socket_transport();
    if (!transport) {
        logger.error("Failed to create transport");
        return 1;
    }

    loom::Context ctx(std::move(transport));

    // Connect to simulation
    logger.info("Connecting to %s...", socket_path.c_str());
    auto rc = ctx.connect(socket_path);
    if (!rc.ok()) {
        logger.error("Failed to connect");
        return 1;
    }

    // Verify DPI function count
    if (ctx.n_dpi_funcs() != static_cast<uint32_t>(loom_dpi_n_funcs)) {
        logger.warning("Design has %u DPI funcs, host expects %d",
                    ctx.n_dpi_funcs(), loom_dpi_n_funcs);
    }

    // Initialize DPI service
    auto& dpi_service = loom::global_dpi_service();
    dpi_service.register_funcs(loom_dpi_funcs, loom_dpi_n_funcs);

    // Run shell
    loom::Shell shell(ctx, dpi_service);
    int exit_code;
    if (!script_file.empty()) {
        exit_code = shell.run_script(script_file);
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

    return exit_code;
}
