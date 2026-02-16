// SPDX-License-Identifier: Apache-2.0
// loom_dpi_service - Generic DPI function dispatch and service loop
//
// This module provides a reusable service loop for handling DPI function
// calls from Loom-instrumented designs. Users register their DPI function
// implementations, then call the service loop.
//
// Usage:
//   1. Include the generated DPI header (e.g., <module>_dpi.h)
//   2. Implement the DPI functions with matching signatures
//   3. Create a dispatch table with loom_dpi_func_t entries
//   4. Call loom_dpi_service_init() with the table
//   5. Call loom_dpi_service_run() to enter the service loop

#ifndef LOOM_DPI_SERVICE_H
#define LOOM_DPI_SERVICE_H

#include "libloom.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of DPI functions supported
#define LOOM_DPI_MAX_FUNCS 64

// Maximum number of arguments per DPI function
#define LOOM_DPI_MAX_ARGS 8

// DPI function callback type
// Args are passed as an array, return value is 64-bit to accommodate all types
typedef uint64_t (*loom_dpi_callback_t)(const uint32_t *args);

// DPI function descriptor
typedef struct {
    int func_id;                    // Function ID (from dpi_bridge)
    const char *name;               // Function name for debugging
    int n_args;                     // Number of arguments
    int ret_width;                  // Return value width in bits (0 for void)
    loom_dpi_callback_t callback;   // User-provided callback
} loom_dpi_func_t;

// Service loop exit codes
typedef enum {
    LOOM_DPI_EXIT_COMPLETE = 0,     // Test completed normally
    LOOM_DPI_EXIT_ERROR = -1,       // Error during service
    LOOM_DPI_EXIT_TIMEOUT = -2,     // Timeout waiting for calls
    LOOM_DPI_EXIT_EMU_ERROR = -3,   // Emulation error state
} loom_dpi_exit_t;

// Initialize DPI service with function table
// funcs: Array of function descriptors
// n_funcs: Number of functions in the array
void loom_dpi_service_init(const loom_dpi_func_t *funcs, int n_funcs);

// Run the DPI service loop
// ctx: Loom context (must be connected and emulation started)
// timeout_ms: Maximum time to wait for DPI calls (0 = infinite)
// Returns: Exit code indicating why the loop terminated
loom_dpi_exit_t loom_dpi_service_run(loom_ctx_t *ctx, int timeout_ms);

// Service a single round of DPI calls (non-blocking)
// Returns number of calls serviced, or negative on error
int loom_dpi_service_once(loom_ctx_t *ctx);

// Get the number of DPI calls serviced since init
uint64_t loom_dpi_service_get_call_count(void);

// Request service loop exit (call from DPI callback to signal test completion)
void loom_dpi_service_request_exit(void);

// Print service statistics
void loom_dpi_service_print_stats(void);

#ifdef __cplusplus
}
#endif

#endif // LOOM_DPI_SERVICE_H
