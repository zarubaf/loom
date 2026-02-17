// SPDX-License-Identifier: Apache-2.0
// Loom Simulation Host - Main Entry Point
//
// This module provides the main() entry point for Loom simulation hosts.
// It handles all the boilerplate: transport setup, connection, emulation
// control, DPI service loop, and cleanup.
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

#include <cstdlib>
#include <string>

// User-provided DPI function table (from generated loom_dpi_dispatch.c)
// loom_dpi_func_t is defined in loom_dpi_service.h
extern "C" {
    extern const loom_dpi_func_t loom_dpi_funcs[];
    extern const int loom_dpi_n_funcs;
}

namespace {
    loom::Logger logger = loom::make_logger("main");
}

int main(int argc, char** argv) {
    std::string socket_path = "/tmp/loom_sim.sock";
    if (argc > 1) {
        socket_path = argv[1];
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

    // Release DUT reset and start emulation
    ctx.dut_reset(false);
    ctx.start();

    // Initial scan capture to demonstrate scan chain functionality
    if (ctx.scan_chain_length() > 0) {
        logger.info("Initial scan capture (%u bits)...", ctx.scan_chain_length());
        auto scan_result = ctx.scan_capture(5000);
        if (scan_result.ok()) {
            auto data_result = ctx.scan_read_data();
            if (data_result.ok()) {
                const auto& scan_data = data_result.value();
                logger.info("Initial state captured (%zu words):", scan_data.size());
                for (size_t i = 0; i < scan_data.size(); i++) {
                    logger.info("  [%2zu] 0x%08x", i, scan_data[i]);
                }
            }
        } else {
            logger.error("Initial scan capture failed");
        }
    }

    // Initialize DPI service
    auto& dpi_service = loom::global_dpi_service();
    dpi_service.register_funcs(loom_dpi_funcs, loom_dpi_n_funcs);

    logger.info("Starting DPI service loop...");
    loom::DpiExitCode exit_code = dpi_service.run(ctx, 30000);  // 30s timeout

    // Get final stats
    auto cycle_result = ctx.get_cycle_count();
    if (cycle_result.ok()) {
        logger.info("Final cycle count: %llu",
                 static_cast<unsigned long long>(cycle_result.value()));
    }
    dpi_service.print_stats();

    // Disconnect
    ctx.disconnect();

    // Report result
    if (exit_code == loom::DpiExitCode::Complete ||
        exit_code == loom::DpiExitCode::Shutdown) {
        logger.info("SIMULATION COMPLETED SUCCESSFULLY (exit=%d)",
                 static_cast<int>(exit_code));
        return 0;
    } else {
        logger.error("SIMULATION FAILED (exit_code=%d)", static_cast<int>(exit_code));
        return 1;
    }
}
