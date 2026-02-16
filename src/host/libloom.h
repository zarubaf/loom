// SPDX-License-Identifier: Apache-2.0
// libloom - Host library for Loom FPGA emulation control
//
// This library provides a transport-agnostic interface for controlling
// Loom-instrumented designs. It supports both socket (simulation) and
// PCIe (FPGA) transports.

#ifndef LIBLOOM_H
#define LIBLOOM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Error codes
// ============================================================================

typedef enum {
    LOOM_OK = 0,
    LOOM_ERR_TRANSPORT = -1,
    LOOM_ERR_TIMEOUT = -2,
    LOOM_ERR_INVALID_ARG = -3,
    LOOM_ERR_NOT_CONNECTED = -4,
    LOOM_ERR_PROTOCOL = -5,
    LOOM_ERR_DPI_ERROR = -6,
    LOOM_ERR_SHUTDOWN = -7,       // Simulation shutdown message received
} loom_error_t;

// ============================================================================
// Emulation states (matches loom_emu_ctrl state machine)
// ============================================================================

typedef enum {
    LOOM_STATE_IDLE = 0,
    LOOM_STATE_RUNNING = 1,
    LOOM_STATE_FROZEN = 2,
    LOOM_STATE_STEPPING = 3,
    LOOM_STATE_SNAPSHOT = 4,
    LOOM_STATE_RESTORE = 5,
    LOOM_STATE_ERROR = 7,
} loom_state_t;

// ============================================================================
// Transport abstraction
// ============================================================================

// Forward declaration
typedef struct loom_transport loom_transport_t;

// Transport operations vtable
typedef struct {
    // Connect to the target (implementation-specific)
    int (*connect)(loom_transport_t *t, const char *target);
    // Disconnect from the target
    void (*disconnect)(loom_transport_t *t);
    // Read a 32-bit register
    int (*read32)(loom_transport_t *t, uint32_t addr, uint32_t *data);
    // Write a 32-bit register
    int (*write32)(loom_transport_t *t, uint32_t addr, uint32_t data);
    // Poll for pending IRQs (returns IRQ mask, 0 if none)
    int (*poll_irq)(loom_transport_t *t, uint32_t *irq_mask, int timeout_ms);
} loom_transport_ops_t;

// Transport instance
struct loom_transport {
    const loom_transport_ops_t *ops;
    void *priv;  // Transport-specific private data
};

// ============================================================================
// Address map constants
// ============================================================================

// Base addresses for different slaves
#define LOOM_ADDR_EMU_CTRL    0x00000
#define LOOM_ADDR_DPI_REGFILE 0x00100

// emu_ctrl register offsets
#define LOOM_EMU_STATUS       0x00
#define LOOM_EMU_CONTROL      0x04
#define LOOM_EMU_CYCLE_LO     0x08
#define LOOM_EMU_CYCLE_HI     0x0C
#define LOOM_EMU_STEP_COUNT   0x10
#define LOOM_EMU_CLK_DIV      0x14
#define LOOM_EMU_DUT_RESET    0x18
#define LOOM_EMU_N_DPI_FUNCS  0x20
#define LOOM_EMU_N_MEMORIES   0x24
#define LOOM_EMU_N_SCAN_CHAINS 0x28
#define LOOM_EMU_TOTAL_SCAN_BITS 0x2C
#define LOOM_EMU_DESIGN_ID    0x34
#define LOOM_EMU_LOOM_VERSION 0x38
#define LOOM_EMU_IRQ_STATUS   0x40
#define LOOM_EMU_IRQ_ENABLE   0x44
#define LOOM_EMU_FINISH       0x4C

// emu_ctrl commands
#define LOOM_CMD_START    0x01
#define LOOM_CMD_STOP     0x02
#define LOOM_CMD_STEP     0x03
#define LOOM_CMD_RESET    0x04
#define LOOM_CMD_SNAPSHOT 0x05
#define LOOM_CMD_RESTORE  0x06

// DPI regfile register offsets (per function, 64 bytes each)
#define LOOM_DPI_FUNC_SIZE    0x40  // 64 bytes per function
#define LOOM_DPI_STATUS       0x00
#define LOOM_DPI_CONTROL      0x04
#define LOOM_DPI_ARG0         0x08
#define LOOM_DPI_ARG1         0x0C
#define LOOM_DPI_ARG2         0x10
#define LOOM_DPI_ARG3         0x14
#define LOOM_DPI_ARG4         0x18
#define LOOM_DPI_ARG5         0x1C
#define LOOM_DPI_ARG6         0x20
#define LOOM_DPI_ARG7         0x24
#define LOOM_DPI_RESULT_LO    0x28
#define LOOM_DPI_RESULT_HI    0x2C

// DPI status bits
#define LOOM_DPI_STATUS_PENDING  (1 << 0)
#define LOOM_DPI_STATUS_DONE     (1 << 1)
#define LOOM_DPI_STATUS_ERROR    (1 << 2)

// DPI control bits
#define LOOM_DPI_CTRL_ACK        (1 << 0)
#define LOOM_DPI_CTRL_SET_DONE   (1 << 1)
#define LOOM_DPI_CTRL_SET_ERROR  (1 << 2)

// ============================================================================
// Loom context
// ============================================================================

typedef struct {
    loom_transport_t *transport;
    uint32_t n_dpi_funcs;
    uint32_t design_id;
    uint32_t loom_version;
} loom_ctx_t;

// ============================================================================
// Core API
// ============================================================================

// Create a new loom context with the given transport
loom_ctx_t *loom_create(loom_transport_t *transport);

// Destroy the loom context (does not free transport)
void loom_destroy(loom_ctx_t *ctx);

// Connect to the target and read design info
int loom_connect(loom_ctx_t *ctx, const char *target);

// Disconnect from the target
void loom_disconnect(loom_ctx_t *ctx);

// ============================================================================
// Emulation control
// ============================================================================

// Get current emulation state
int loom_get_state(loom_ctx_t *ctx, loom_state_t *state);

// Start emulation (enter RUNNING state)
int loom_start(loom_ctx_t *ctx);

// Stop emulation (enter FROZEN state)
int loom_stop(loom_ctx_t *ctx);

// Step N cycles (enter STEPPING then FROZEN)
int loom_step(loom_ctx_t *ctx, uint32_t n_cycles);

// Reset emulation (return to IDLE state)
int loom_reset(loom_ctx_t *ctx);

// Get current cycle count
int loom_get_cycle_count(loom_ctx_t *ctx, uint64_t *count);

// Assert/deassert DUT reset
int loom_dut_reset(loom_ctx_t *ctx, int assert_reset);

// Request shutdown with exit code (triggers BFM $finish in sim)
int loom_finish(loom_ctx_t *ctx, int exit_code);

// ============================================================================
// DPI function handling
// ============================================================================

// DPI call information
typedef struct {
    uint32_t func_id;
    uint32_t args[8];
    int n_args;
} loom_dpi_call_t;

// Check if any DPI call is pending
int loom_dpi_poll(loom_ctx_t *ctx, uint32_t *pending_mask);

// Get details of a pending DPI call
int loom_dpi_get_call(loom_ctx_t *ctx, uint32_t func_id, loom_dpi_call_t *call);

// Complete a DPI call with a return value
int loom_dpi_complete(loom_ctx_t *ctx, uint32_t func_id, uint64_t result);

// Complete a DPI call with an error
int loom_dpi_error(loom_ctx_t *ctx, uint32_t func_id);

// ============================================================================
// Low-level register access
// ============================================================================

// Read a 32-bit register
int loom_read32(loom_ctx_t *ctx, uint32_t addr, uint32_t *data);

// Write a 32-bit register
int loom_write32(loom_ctx_t *ctx, uint32_t addr, uint32_t data);

// ============================================================================
// Transport factory functions
// ============================================================================

// Create a socket transport (for simulation)
loom_transport_t *loom_transport_socket_create(void);

// Destroy a socket transport
void loom_transport_socket_destroy(loom_transport_t *t);

// Create a PCIe transport (for FPGA) - placeholder for future
// loom_transport_t *loom_transport_pcie_create(void);

#ifdef __cplusplus
}
#endif

#endif // LIBLOOM_H
