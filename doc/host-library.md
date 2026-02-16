<!-- SPDX-License-Identifier: Apache-2.0 -->
# Host Library

The Loom host library provides C APIs for communicating with an emulated design and servicing DPI function calls.

## Components

```
src/host/
├── libloom.h              # Main API header
├── libloom.c              # Core library implementation
├── loom_transport_socket.h/c  # Unix socket transport
└── loom_dpi_service.h/c   # Generic DPI service loop
```

## libloom API

### Connection

```c
#include "libloom.h"

// Create transport and context
loom_transport_t *transport = loom_transport_socket_create();
loom_ctx_t *ctx = loom_create(transport);

// Connect to simulation
int rc = loom_connect(ctx, "/tmp/loom_sim.sock");
if (rc == LOOM_OK) {
    printf("Design ID: 0x%08x\n", ctx->design_id);
    printf("N_DPI_FUNCS: %u\n", ctx->n_dpi_funcs);
}

// Cleanup
loom_disconnect(ctx);
loom_destroy(ctx);
loom_transport_socket_destroy(transport);
```

### Emulation Control

```c
// Release DUT reset
loom_dut_reset(ctx, 0);

// Start emulation
loom_start(ctx);

// Get state
loom_state_t state;
loom_get_state(ctx, &state);
// state: LOOM_STATE_IDLE, LOOM_STATE_RUNNING, LOOM_STATE_FROZEN

// Get cycle count
uint64_t cycles;
loom_get_cycle_count(ctx, &cycles);
```

### DPI Polling

```c
// Poll for pending DPI calls
uint32_t pending_mask;
loom_dpi_poll(ctx, &pending_mask);

// Get call details for function 0
if (pending_mask & (1 << 0)) {
    loom_dpi_call_t call;
    loom_dpi_get_call(ctx, 0, &call);
    // call.args[0], call.args[1], ...
}

// Complete a call with result
loom_dpi_complete(ctx, func_id, result);
```

## DPI Service Library

For typical use cases, the generic DPI service handles the polling loop automatically.

### Setup

```c
#include "loom_dpi_service.h"
#include "my_design_dpi.h"  // Generated header

// Implement the DPI functions
int32_t dpi_add(int32_t a, int32_t b) {
    return a + b;
}

int32_t dpi_report_result(int32_t passed, int32_t result) {
    printf("Test %s: result=%d\n", passed ? "PASSED" : "FAILED", result);
    loom_dpi_service_request_exit();  // Signal completion
    return 0;
}

// Wrapper callbacks (adapt typed functions to generic signature)
static uint64_t wrap_dpi_add(const uint32_t *args) {
    return (uint64_t)dpi_add((int32_t)args[0], (int32_t)args[1]);
}

static uint64_t wrap_dpi_report_result(const uint32_t *args) {
    return (uint64_t)dpi_report_result((int32_t)args[0], (int32_t)args[1]);
}

// Function table
static const loom_dpi_func_t dpi_funcs[] = {
    { DPI_FUNC_DPI_REPORT_RESULT, "dpi_report_result", 2, 32, wrap_dpi_report_result },
    { DPI_FUNC_DPI_ADD, "dpi_add", 2, 32, wrap_dpi_add },
};
```

### Running the Service Loop

```c
// Initialize service
loom_dpi_service_init(dpi_funcs, DPI_N_FUNCS);

// Run until completion or timeout
loom_dpi_exit_t exit_code = loom_dpi_service_run(ctx, 30000);  // 30s timeout

// Print statistics
loom_dpi_service_print_stats();
```

### Exit Conditions

The service loop exits when:

1. `loom_dpi_service_request_exit()` is called from a callback
2. Emulation enters FROZEN or ERROR state
3. Timeout expires with no DPI activity

## Transport Layer

The transport layer abstracts the communication mechanism. Currently supported:

### Unix Socket Transport

```c
loom_transport_t *transport = loom_transport_socket_create();
// ... use with loom_create() ...
loom_transport_socket_destroy(transport);
```

The socket transport connects to a Verilator simulation running `loom_axil_socket_bfm`.

### Future Transports

- PCIe (XDMA/QDMA) for FPGA deployment
- Shared memory for fast local simulation
