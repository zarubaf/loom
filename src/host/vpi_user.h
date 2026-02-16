// SPDX-License-Identifier: Apache-2.0
// Loom VPI implementation
//
// This provides a subset of the standard VPI (Verilog Procedural Interface)
// that works with Loom's socket-based simulation architecture.
//
// Supported functions:
//   vpi_control(vpiFinish, exit_code) - Terminate simulation
//   vpi_control(vpiStop, ...)         - Pause simulation (not yet implemented)
//   vpi_printf(...)                   - Print to simulation log

#ifndef VPI_USER_H
#define VPI_USER_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// VPI control operations
#define vpiStop    66
#define vpiFinish  67

// vpi_control - Control simulation execution
// op: vpiFinish to terminate, vpiStop to pause
// Returns 0 on success
int vpi_control(int op, ...);

// vpi_printf - Print formatted output
// Returns number of characters printed
int vpi_printf(const char *fmt, ...);

// Internal: Set context for VPI functions (called by loom_dpi_service)
#include "libloom.h"
void loom_vpi_set_context(loom_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif // VPI_USER_H
