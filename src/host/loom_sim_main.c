// SPDX-License-Identifier: Apache-2.0
// loom_sim_main.c - Generic simulation host main entry point
//
// This module provides the main() entry point for Loom simulation hosts.
// It handles all the boilerplate: transport setup, connection, emulation
// control, DPI service loop, and cleanup.
//
// Usage:
//   1. Include the generated DPI header (e.g., <module>_dpi.h)
//   2. Define your DPI function table: loom_dpi_funcs and loom_dpi_n_funcs
//   3. Link this file with your dpi_impl.c
//
// The user must provide:
//   extern const loom_dpi_func_t loom_dpi_funcs[];
//   extern const int loom_dpi_n_funcs;

#include <stdio.h>
#include <stdlib.h>
#include "libloom.h"
#include "loom_dpi_service.h"

// User-provided DPI function table
extern const loom_dpi_func_t loom_dpi_funcs[];
extern const int loom_dpi_n_funcs;

int main(int argc, char **argv) {
    const char *socket_path = "/tmp/loom_sim.sock";
    if (argc > 1) {
        socket_path = argv[1];
    }

    printf("[loom] Loom Simulation Host\n");
    printf("[loom] Socket: %s\n", socket_path);

    // Create transport and context
    loom_transport_t *transport = loom_transport_socket_create();
    if (!transport) {
        fprintf(stderr, "[loom] Failed to create transport\n");
        return 1;
    }

    loom_ctx_t *ctx = loom_create(transport);
    if (!ctx) {
        fprintf(stderr, "[loom] Failed to create context\n");
        loom_transport_socket_destroy(transport);
        return 1;
    }

    // Connect to simulation
    printf("[loom] Connecting to %s...\n", socket_path);
    int rc = loom_connect(ctx, socket_path);
    if (rc != LOOM_OK) {
        fprintf(stderr, "[loom] Failed to connect: %d\n", rc);
        loom_destroy(ctx);
        loom_transport_socket_destroy(transport);
        return 1;
    }

    printf("[loom] Connected. Design ID: 0x%08x, Version: 0x%08x, N_DPI: %u\n",
           ctx->design_id, ctx->loom_version, ctx->n_dpi_funcs);

    // Verify DPI function count
    if (ctx->n_dpi_funcs != (uint32_t)loom_dpi_n_funcs) {
        fprintf(stderr, "[loom] WARNING: Design has %u DPI funcs, host expects %d\n",
                ctx->n_dpi_funcs, loom_dpi_n_funcs);
    }

    // Release DUT reset and start emulation
    loom_dut_reset(ctx, 0);  // Deassert reset
    loom_start(ctx);

    // Initialize and run DPI service
    loom_dpi_service_init(loom_dpi_funcs, loom_dpi_n_funcs);

    printf("[loom] Starting DPI service loop...\n");
    loom_dpi_exit_t exit_code = loom_dpi_service_run(ctx, 30000);  // 30s timeout

    // Get final stats
    uint64_t cycle_count;
    loom_get_cycle_count(ctx, &cycle_count);
    printf("[loom] Final cycle count: %llu\n", (unsigned long long)cycle_count);
    loom_dpi_service_print_stats();

    // Cleanup
    loom_disconnect(ctx);
    loom_destroy(ctx);
    loom_transport_socket_destroy(transport);

    // Report result
    if (exit_code == LOOM_DPI_EXIT_COMPLETE || exit_code == LOOM_DPI_EXIT_SHUTDOWN) {
        printf("[loom] SIMULATION COMPLETED SUCCESSFULLY (exit=%d)\n", exit_code);
        return 0;
    } else {
        printf("[loom] SIMULATION FAILED (exit_code=%d)\n", exit_code);
        return 1;
    }
}
