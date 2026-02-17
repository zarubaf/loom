// SPDX-License-Identifier: Apache-2.0
// Loom DPI Service - Generic DPI function dispatch and service loop
//
// This header provides:
// - C-compatible loom_dpi_func_t struct for generated dispatch tables
// - C++ DpiService class for the service loop (when compiled as C++)

#ifndef LOOM_DPI_SERVICE_H
#define LOOM_DPI_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// C-compatible definitions (used by generated loom_dpi_dispatch.c)
// ============================================================================

// DPI function callback type
// Args are passed as an array, return value is 64-bit to accommodate all types
typedef uint64_t (*loom_dpi_callback_t)(const uint32_t *args);

// DPI function descriptor
typedef struct {
    int func_id;                    // Function ID (from loom_instrument)
    const char *name;               // Function name for debugging
    int n_args;                     // Number of arguments
    int ret_width;                  // Return value width in bits (0 for void)
    loom_dpi_callback_t callback;   // User-provided callback
} loom_dpi_func_t;

#ifdef __cplusplus
}
#endif

// ============================================================================
// C++ definitions
// ============================================================================

#ifdef __cplusplus

#include "loom.h"
#include <string>
#include <vector>
#include <functional>
#include <span>

namespace loom {

// Maximum number of arguments per DPI function
constexpr int kDpiMaxArgs = 8;

// DPI function callback type
// Args are passed as a span, return value is 64-bit to accommodate all types
using DpiCallback = std::function<uint64_t(std::span<const uint32_t> args)>;

// DPI function descriptor
struct DpiFunc {
    int func_id;                // Function ID (from loom_instrument)
    std::string name;           // Function name for debugging
    int n_args;                 // Number of arguments
    int ret_width;              // Return value width in bits (0 for void)
    DpiCallback callback;       // User-provided callback
};

// Service loop exit codes
enum class DpiExitCode {
    Complete = 0,       // Test completed normally (shutdown received)
    Error = -1,         // Error during service
    Timeout = -2,       // Timeout waiting for calls
    EmuError = -3,      // Emulation error state
    Shutdown = 1,       // Explicit shutdown from simulation
};

// ============================================================================
// DPI Service Class
// ============================================================================

class DpiService {
public:
    DpiService() = default;
    ~DpiService() = default;

    // Non-copyable
    DpiService(const DpiService&) = delete;
    DpiService& operator=(const DpiService&) = delete;

    // Register a DPI function
    void register_func(int func_id, std::string_view name, int n_args,
                       int ret_width, DpiCallback callback);

    // Register functions from a C-style array (for compatibility with generated code)
    template<typename T>
    void register_funcs(const T* funcs, int n_funcs) {
        for (int i = 0; i < n_funcs; i++) {
            register_func(funcs[i].func_id, funcs[i].name, funcs[i].n_args,
                          funcs[i].ret_width,
                          [cb = funcs[i].callback](std::span<const uint32_t> args) {
                              return cb(args.data());
                          });
        }
    }

    // Run the DPI service loop
    // ctx: Loom context (must be connected and emulation started)
    // timeout_ms: Maximum time to wait for DPI calls (0 = infinite)
    // Returns: Exit code indicating why the loop terminated
    DpiExitCode run(Context& ctx, int timeout_ms = 0);

    // Service a single round of DPI calls (non-blocking)
    // Returns number of calls serviced, or negative on error
    int service_once(Context& ctx);

    // Accessors
    uint64_t call_count() const { return call_count_; }
    uint64_t error_count() const { return error_count_; }
    size_t func_count() const { return funcs_.size(); }

    // Print service statistics
    void print_stats() const;

    // Get current context (for VPI functions)
    Context* current_context() const { return current_ctx_; }

private:
    const DpiFunc* find_func(int func_id) const;

    std::vector<DpiFunc> funcs_;
    uint64_t call_count_ = 0;
    uint64_t error_count_ = 0;
    Context* current_ctx_ = nullptr;
};

// Global DPI service instance (for VPI compatibility)
DpiService& global_dpi_service();

} // namespace loom

#endif // __cplusplus

#endif // LOOM_DPI_SERVICE_H
