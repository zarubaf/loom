// SPDX-License-Identifier: Apache-2.0
// Loom VPI implementation

#include "vpi_user.h"
#include "libloom.h"
#include <stdio.h>
#include <stdarg.h>

// Context set by loom_dpi_service_run
static loom_ctx_t *g_vpi_ctx = NULL;

void loom_vpi_set_context(loom_ctx_t *ctx) {
    g_vpi_ctx = ctx;
}

int vpi_control(int op, ...) {
    va_list args;
    va_start(args, op);

    int result = 0;

    switch (op) {
    case vpiFinish: {
        int exit_code = va_arg(args, int);
        if (g_vpi_ctx) {
            loom_finish(g_vpi_ctx, exit_code);
        }
        break;
    }
    case vpiStop:
        if (g_vpi_ctx) {
            loom_stop(g_vpi_ctx);
        }
        break;
    default:
        result = -1;
        break;
    }

    va_end(args);
    return result;
}

int vpi_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}
