// SPDX-License-Identifier: Apache-2.0
// Test harness for dpi_test e2e test
//
// This demonstrates how to use the generic DPI service loop with
// user-provided DPI function implementations.

#include <stdio.h>
#include <stdlib.h>
#include "libloom.h"
#include "loom_dpi_service.h"
#include "dpi_test_dpi.h"

// ============================================================================
// DPI callback wrappers
// The generic service uses a uniform callback signature. These wrappers
// adapt the user's type-safe functions to the callback interface.
// ============================================================================

static uint64_t wrap_dpi_add(const uint32_t *args) {
    return (uint64_t)dpi_add((int32_t)args[0], (int32_t)args[1]);
}

static uint64_t wrap_dpi_report_result(const uint32_t *args) {
    return (uint64_t)dpi_report_result((int32_t)args[0], (int32_t)args[1]);
}

// ============================================================================
// DPI function table
// ============================================================================

static const loom_dpi_func_t dpi_funcs[] = {
    { DPI_FUNC_DPI_REPORT_RESULT, "dpi_report_result", 2, 32, wrap_dpi_report_result },
    { DPI_FUNC_DPI_ADD, "dpi_add", 2, 32, wrap_dpi_add },
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    const char *socket_path = "/tmp/loom_sim.sock";
    if (argc > 1) {
        socket_path = argv[1];
    }

    printf("[test] Loom E2E Test Harness\n");
    printf("[test] Socket: %s\n", socket_path);

    // Create transport and context
    loom_transport_t *transport = loom_transport_socket_create();
    if (!transport) {
        fprintf(stderr, "[test] Failed to create transport\n");
        return 1;
    }

    loom_ctx_t *ctx = loom_create(transport);
    if (!ctx) {
        fprintf(stderr, "[test] Failed to create context\n");
        loom_transport_socket_destroy(transport);
        return 1;
    }

    // Connect to simulation
    printf("[test] Connecting to %s...\n", socket_path);
    int rc = loom_connect(ctx, socket_path);
    if (rc != LOOM_OK) {
        fprintf(stderr, "[test] Failed to connect: %d\n", rc);
        loom_destroy(ctx);
        loom_transport_socket_destroy(transport);
        return 1;
    }

    printf("[test] Connected. Design ID: 0x%08x, Version: 0x%08x, N_DPI: %u\n",
           ctx->design_id, ctx->loom_version, ctx->n_dpi_funcs);

    // Verify DPI function count
    if (ctx->n_dpi_funcs != DPI_N_FUNCS) {
        fprintf(stderr, "[test] WARNING: Design has %u DPI funcs, expected %d\n",
                ctx->n_dpi_funcs, DPI_N_FUNCS);
    }

    // Release DUT reset and start emulation
    loom_dut_reset(ctx, 0);  // Deassert reset
    loom_start(ctx);

    // Initialize DPI service
    loom_dpi_service_init(dpi_funcs, DPI_N_FUNCS);

    // Run service loop
    printf("[test] Starting DPI service loop...\n");
    loom_dpi_exit_t exit_code = loom_dpi_service_run(ctx, 30000);  // 30s timeout

    // Get final stats
    uint64_t cycle_count;
    loom_get_cycle_count(ctx, &cycle_count);
    printf("[test] Final cycle count: %llu\n", (unsigned long long)cycle_count);
    loom_dpi_service_print_stats();

    // Cleanup
    loom_disconnect(ctx);
    loom_destroy(ctx);
    loom_transport_socket_destroy(transport);

    // Report result
    if (exit_code == LOOM_DPI_EXIT_COMPLETE) {
        printf("[test] TEST COMPLETED SUCCESSFULLY\n");
        return 0;
    } else {
        printf("[test] TEST FAILED (exit_code=%d)\n", exit_code);
        return 1;
    }
}
