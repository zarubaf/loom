// SPDX-License-Identifier: Apache-2.0
// loom_dpi_service - Generic DPI function dispatch and service loop

#include "loom_dpi_service.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Service state
static const loom_dpi_func_t *g_funcs = NULL;
static int g_n_funcs = 0;
static uint64_t g_call_count = 0;
static uint64_t g_error_count = 0;
static int g_exit_requested = 0;

void loom_dpi_service_init(const loom_dpi_func_t *funcs, int n_funcs) {
    g_funcs = funcs;
    g_n_funcs = n_funcs;
    g_call_count = 0;
    g_error_count = 0;
    g_exit_requested = 0;
}

static const loom_dpi_func_t *find_func(int func_id) {
    for (int i = 0; i < g_n_funcs; i++) {
        if (g_funcs[i].func_id == func_id) {
            return &g_funcs[i];
        }
    }
    return NULL;
}

int loom_dpi_service_once(loom_ctx_t *ctx) {
    int rc;
    uint32_t pending_mask;
    int serviced = 0;

    // Poll for pending DPI calls
    rc = loom_dpi_poll(ctx, &pending_mask);
    if (rc != LOOM_OK) {
        fprintf(stderr, "[dpi_service] Poll failed: %d\n", rc);
        return rc;
    }

    if (pending_mask == 0) {
        return 0;  // No pending calls
    }

    // Service each pending call
    for (int func_id = 0; func_id < g_n_funcs && func_id < 32; func_id++) {
        if (!(pending_mask & (1 << func_id))) {
            continue;
        }

        const loom_dpi_func_t *func = find_func(func_id);
        if (!func) {
            fprintf(stderr, "[dpi_service] Unknown function ID: %d\n", func_id);
            loom_dpi_error(ctx, func_id);
            g_error_count++;
            continue;
        }

        if (!func->callback) {
            fprintf(stderr, "[dpi_service] No callback for function '%s' (id=%d)\n",
                    func->name, func_id);
            loom_dpi_error(ctx, func_id);
            g_error_count++;
            continue;
        }

        // Get call details
        loom_dpi_call_t call;
        rc = loom_dpi_get_call(ctx, func_id, &call);
        if (rc != LOOM_OK) {
            fprintf(stderr, "[dpi_service] Failed to get call for '%s': %d\n",
                    func->name, rc);
            g_error_count++;
            continue;
        }

        // Call the user function
        uint64_t result = func->callback(call.args);

        // Complete the call
        rc = loom_dpi_complete(ctx, func_id, result);
        if (rc != LOOM_OK) {
            fprintf(stderr, "[dpi_service] Failed to complete call for '%s': %d\n",
                    func->name, rc);
            g_error_count++;
            continue;
        }

        serviced++;
        g_call_count++;
    }

    return serviced;
}

void loom_dpi_service_request_exit(void) {
    g_exit_requested = 1;
}

loom_dpi_exit_t loom_dpi_service_run(loom_ctx_t *ctx, int timeout_ms) {
    int rc;
    int no_activity_count = 0;
    const int max_no_activity = timeout_ms > 0 ? (timeout_ms / 10) : 1000;

    printf("[dpi_service] Entering service loop (n_funcs=%d)\n", g_n_funcs);

    while (1) {
        // Service pending calls
        rc = loom_dpi_service_once(ctx);
        if (rc < 0) {
            return LOOM_DPI_EXIT_ERROR;
        }

        if (rc > 0) {
            no_activity_count = 0;  // Reset timeout counter
        } else {
            no_activity_count++;
        }

        // Check if a DPI callback requested exit
        if (g_exit_requested) {
            printf("[dpi_service] Exit requested by DPI callback\n");
            return LOOM_DPI_EXIT_COMPLETE;
        }

        // Check emulation state
        loom_state_t state;
        rc = loom_get_state(ctx, &state);
        if (rc != LOOM_OK) {
            fprintf(stderr, "[dpi_service] Failed to get state: %d\n", rc);
            return LOOM_DPI_EXIT_ERROR;
        }

        // Exit on error state
        if (state == LOOM_STATE_ERROR) {
            fprintf(stderr, "[dpi_service] Emulation error state\n");
            return LOOM_DPI_EXIT_EMU_ERROR;
        }

        // Exit on frozen state (test complete)
        if (state == LOOM_STATE_FROZEN) {
            printf("[dpi_service] Emulation frozen, test complete\n");
            return LOOM_DPI_EXIT_COMPLETE;
        }

        // Timeout check - if no DPI activity for a while and we've done some calls
        if (g_call_count > 0 && no_activity_count >= max_no_activity) {
            printf("[dpi_service] No DPI activity, assuming test complete\n");
            return LOOM_DPI_EXIT_COMPLETE;
        }

        // Small delay to avoid busy-waiting
        usleep(10000);  // 10ms
    }
}

uint64_t loom_dpi_service_get_call_count(void) {
    return g_call_count;
}

void loom_dpi_service_print_stats(void) {
    printf("[dpi_service] Statistics:\n");
    printf("  Total calls serviced: %llu\n", (unsigned long long)g_call_count);
    printf("  Errors: %llu\n", (unsigned long long)g_error_count);
    printf("  Registered functions: %d\n", g_n_funcs);
    for (int i = 0; i < g_n_funcs; i++) {
        printf("    [%d] %s (%d args, %d-bit return)\n",
               g_funcs[i].func_id, g_funcs[i].name,
               g_funcs[i].n_args, g_funcs[i].ret_width);
    }
}
