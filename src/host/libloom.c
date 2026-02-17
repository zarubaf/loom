// SPDX-License-Identifier: Apache-2.0
// libloom - Host library implementation

#include "libloom.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Context management
// ============================================================================

loom_ctx_t *loom_create(loom_transport_t *transport) {
    if (!transport) return NULL;

    loom_ctx_t *ctx = (loom_ctx_t *)calloc(1, sizeof(loom_ctx_t));
    if (!ctx) return NULL;

    ctx->transport = transport;
    return ctx;
}

void loom_destroy(loom_ctx_t *ctx) {
    if (ctx) {
        free(ctx);
    }
}

int loom_connect(loom_ctx_t *ctx, const char *target) {
    if (!ctx || !ctx->transport || !ctx->transport->ops) {
        return LOOM_ERR_INVALID_ARG;
    }

    int rc = ctx->transport->ops->connect(ctx->transport, target);
    if (rc != LOOM_OK) {
        return rc;
    }

    // Read design info
    uint32_t val;
    rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_N_DPI_FUNCS, &val);
    if (rc != LOOM_OK) return rc;
    ctx->n_dpi_funcs = val;

    rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_TOTAL_SCAN_BITS, &val);
    if (rc != LOOM_OK) return rc;
    ctx->scan_chain_length = val;

    rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_DESIGN_ID, &val);
    if (rc != LOOM_OK) return rc;
    ctx->design_id = val;

    rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_LOOM_VERSION, &val);
    if (rc != LOOM_OK) return rc;
    ctx->loom_version = val;

    return LOOM_OK;
}

void loom_disconnect(loom_ctx_t *ctx) {
    if (ctx && ctx->transport && ctx->transport->ops) {
        ctx->transport->ops->disconnect(ctx->transport);
    }
}

// ============================================================================
// Low-level register access
// ============================================================================

int loom_read32(loom_ctx_t *ctx, uint32_t addr, uint32_t *data) {
    if (!ctx || !ctx->transport || !ctx->transport->ops || !data) {
        return LOOM_ERR_INVALID_ARG;
    }
    return ctx->transport->ops->read32(ctx->transport, addr, data);
}

int loom_write32(loom_ctx_t *ctx, uint32_t addr, uint32_t data) {
    if (!ctx || !ctx->transport || !ctx->transport->ops) {
        return LOOM_ERR_INVALID_ARG;
    }
    return ctx->transport->ops->write32(ctx->transport, addr, data);
}

// ============================================================================
// Emulation control
// ============================================================================

int loom_get_state(loom_ctx_t *ctx, loom_state_t *state) {
    if (!ctx || !state) return LOOM_ERR_INVALID_ARG;

    uint32_t val;
    int rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_STATUS, &val);
    if (rc != LOOM_OK) return rc;

    *state = (loom_state_t)(val & 0x7);
    return LOOM_OK;
}

int loom_start(loom_ctx_t *ctx) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;
    return loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_CONTROL, LOOM_CMD_START);
}

int loom_stop(loom_ctx_t *ctx) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;
    return loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_CONTROL, LOOM_CMD_STOP);
}

int loom_step(loom_ctx_t *ctx, uint32_t n_cycles) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;

    // Set step count first
    int rc = loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_STEP_COUNT, n_cycles);
    if (rc != LOOM_OK) return rc;

    // Issue step command
    return loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_CONTROL, LOOM_CMD_STEP);
}

int loom_reset(loom_ctx_t *ctx) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;
    return loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_CONTROL, LOOM_CMD_RESET);
}

int loom_get_cycle_count(loom_ctx_t *ctx, uint64_t *count) {
    if (!ctx || !count) return LOOM_ERR_INVALID_ARG;

    uint32_t lo, hi;
    int rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_CYCLE_LO, &lo);
    if (rc != LOOM_OK) return rc;

    rc = loom_read32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_CYCLE_HI, &hi);
    if (rc != LOOM_OK) return rc;

    *count = ((uint64_t)hi << 32) | lo;
    return LOOM_OK;
}

int loom_dut_reset(loom_ctx_t *ctx, int assert_reset) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;

    // Bit 0: assert reset, Bit 1: release reset
    uint32_t val = assert_reset ? 0x1 : 0x2;
    return loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_DUT_RESET, val);
}

int loom_finish(loom_ctx_t *ctx, int exit_code) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;

    // [0] = finish_req (1), [15:8] = exit_code
    uint32_t val = 0x01 | ((exit_code & 0xFF) << 8);
    return loom_write32(ctx, LOOM_ADDR_EMU_CTRL + LOOM_EMU_FINISH, val);
}

// ============================================================================
// DPI function handling
// ============================================================================

// Helper to compute DPI function base address
static inline uint32_t dpi_func_addr(uint32_t func_id, uint32_t reg_offset) {
    return LOOM_ADDR_DPI_REGFILE + (func_id * LOOM_DPI_FUNC_SIZE) + reg_offset;
}

int loom_dpi_poll(loom_ctx_t *ctx, uint32_t *pending_mask) {
    if (!ctx || !pending_mask) return LOOM_ERR_INVALID_ARG;

    *pending_mask = 0;

    // Check each DPI function's status
    for (uint32_t i = 0; i < ctx->n_dpi_funcs; i++) {
        uint32_t status;
        int rc = loom_read32(ctx, dpi_func_addr(i, LOOM_DPI_STATUS), &status);
        if (rc != LOOM_OK) return rc;

        if (status & LOOM_DPI_STATUS_PENDING) {
            *pending_mask |= (1 << i);
        }
    }

    return LOOM_OK;
}

int loom_dpi_get_call(loom_ctx_t *ctx, uint32_t func_id, loom_dpi_call_t *call) {
    if (!ctx || !call) return LOOM_ERR_INVALID_ARG;
    if (func_id >= ctx->n_dpi_funcs) return LOOM_ERR_INVALID_ARG;

    call->func_id = func_id;

    // Read status first to confirm it's pending
    uint32_t status;
    int rc = loom_read32(ctx, dpi_func_addr(func_id, LOOM_DPI_STATUS), &status);
    if (rc != LOOM_OK) return rc;

    if (!(status & LOOM_DPI_STATUS_PENDING)) {
        return LOOM_ERR_PROTOCOL;  // Not pending
    }

    // Read all arguments
    for (int i = 0; i < 8; i++) {
        rc = loom_read32(ctx, dpi_func_addr(func_id, LOOM_DPI_ARG0 + i * 4), &call->args[i]);
        if (rc != LOOM_OK) return rc;
    }

    // n_args is not tracked here; caller should know the function signature
    call->n_args = 8;

    return LOOM_OK;
}

int loom_dpi_complete(loom_ctx_t *ctx, uint32_t func_id, uint64_t result) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;
    if (func_id >= ctx->n_dpi_funcs) return LOOM_ERR_INVALID_ARG;

    // Write result (low then high)
    int rc = loom_write32(ctx, dpi_func_addr(func_id, LOOM_DPI_RESULT_LO),
                          (uint32_t)(result & 0xFFFFFFFF));
    if (rc != LOOM_OK) return rc;

    rc = loom_write32(ctx, dpi_func_addr(func_id, LOOM_DPI_RESULT_HI),
                      (uint32_t)(result >> 32));
    if (rc != LOOM_OK) return rc;

    // Set done flag
    rc = loom_write32(ctx, dpi_func_addr(func_id, LOOM_DPI_CONTROL), LOOM_DPI_CTRL_SET_DONE);
    if (rc != LOOM_OK) return rc;

    return LOOM_OK;
}

int loom_dpi_error(loom_ctx_t *ctx, uint32_t func_id) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;
    if (func_id >= ctx->n_dpi_funcs) return LOOM_ERR_INVALID_ARG;

    // Set error and done flags
    return loom_write32(ctx, dpi_func_addr(func_id, LOOM_DPI_CONTROL),
                        LOOM_DPI_CTRL_SET_DONE | LOOM_DPI_CTRL_SET_ERROR);
}

// ============================================================================
// Scan chain control
// ============================================================================

int loom_scan_get_length(loom_ctx_t *ctx, uint32_t *length) {
    if (!ctx || !length) return LOOM_ERR_INVALID_ARG;
    *length = ctx->scan_chain_length;
    return LOOM_OK;
}

int loom_scan_is_busy(loom_ctx_t *ctx, int *busy) {
    if (!ctx || !busy) return LOOM_ERR_INVALID_ARG;

    uint32_t status;
    int rc = loom_read32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_STATUS, &status);
    if (rc != LOOM_OK) return rc;

    *busy = (status & LOOM_SCAN_STATUS_BUSY) != 0;
    return LOOM_OK;
}

int loom_scan_clear_done(loom_ctx_t *ctx) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;

    // Write to status register to clear done flag (write 1 to bit 1)
    return loom_write32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_STATUS, LOOM_SCAN_STATUS_DONE);
}

// Helper: wait for scan operation to complete
static int scan_wait_done(loom_ctx_t *ctx, int timeout_ms) {
    int elapsed = 0;
    const int poll_interval = 1;  // ms

    while (elapsed < timeout_ms) {
        uint32_t status;
        int rc = loom_read32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_STATUS, &status);
        if (rc != LOOM_OK) return rc;

        if (status & LOOM_SCAN_STATUS_DONE) {
            return LOOM_OK;
        }

        // Simple busy-wait; in real implementation would use usleep
        // For now, just do more reads as a delay
        for (volatile int i = 0; i < 1000; i++);
        elapsed += poll_interval;
    }

    return LOOM_ERR_TIMEOUT;
}

int loom_scan_capture(loom_ctx_t *ctx, int timeout_ms) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;

    // Clear any previous done flag
    int rc = loom_scan_clear_done(ctx);
    if (rc != LOOM_OK) return rc;

    // Issue capture command
    rc = loom_write32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_CONTROL, LOOM_SCAN_CMD_CAPTURE);
    if (rc != LOOM_OK) return rc;

    // Wait for completion
    return scan_wait_done(ctx, timeout_ms);
}

int loom_scan_restore(loom_ctx_t *ctx, int timeout_ms) {
    if (!ctx) return LOOM_ERR_INVALID_ARG;

    // Clear any previous done flag
    int rc = loom_scan_clear_done(ctx);
    if (rc != LOOM_OK) return rc;

    // Issue restore command
    rc = loom_write32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_CONTROL, LOOM_SCAN_CMD_RESTORE);
    if (rc != LOOM_OK) return rc;

    // Wait for completion
    return scan_wait_done(ctx, timeout_ms);
}

int loom_scan_read_data(loom_ctx_t *ctx, uint32_t *data, size_t max_words) {
    if (!ctx || !data) return LOOM_ERR_INVALID_ARG;

    // Calculate how many words we need
    uint32_t n_words = (ctx->scan_chain_length + 31) / 32;
    if (n_words > max_words) n_words = max_words;

    // Read each word
    for (uint32_t i = 0; i < n_words; i++) {
        int rc = loom_read32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_DATA_BASE + (i * 4), &data[i]);
        if (rc != LOOM_OK) return rc;
    }

    return (int)n_words;
}

int loom_scan_write_data(loom_ctx_t *ctx, const uint32_t *data, size_t n_words) {
    if (!ctx || !data) return LOOM_ERR_INVALID_ARG;

    // Write each word
    for (size_t i = 0; i < n_words; i++) {
        int rc = loom_write32(ctx, LOOM_ADDR_SCAN_CTRL + LOOM_SCAN_DATA_BASE + (i * 4), data[i]);
        if (rc != LOOM_OK) return rc;
    }

    return LOOM_OK;
}
