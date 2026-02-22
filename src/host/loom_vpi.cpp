// SPDX-License-Identifier: Apache-2.0
// Loom VPI Implementation

#include "vpi_user.h"
#include "loom_dpi_service.h"
#include "loom_log.h"
#include <cstdio>
#include <cstdarg>

namespace {
    loom::Logger logger = loom::make_logger("vpi");
}

extern "C" {

int vpi_control(int op, ...) {
    va_list args;
    va_start(args, op);

    int result = 0;

    loom::Context* ctx = loom::global_dpi_service().current_context();

    switch (op) {
    case vpiFinish: {
        int exit_code = va_arg(args, int);
        if (ctx) {
            logger.info("vpi_control(vpiFinish, %d)", exit_code);
            ctx->finish(exit_code);
        } else {
            logger.warning("vpi_control(vpiFinish, %d) called without context", exit_code);
        }
        break;
    }
    case vpiStop:
        if (ctx) {
            logger.debug("vpi_control(vpiStop)");
            ctx->stop();
        }
        break;
    default:
        logger.warning("Unknown vpi_control operation: %d", op);
        result = -1;
        break;
    }

    va_end(args);
    return result;
}

int vpi_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = std::vprintf(fmt, args);
    va_end(args);
    return ret;
}

}
