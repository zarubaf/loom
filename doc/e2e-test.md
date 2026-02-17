<!-- SPDX-License-Identifier: Apache-2.0 -->
# End-to-End Test

The e2e test in `tests/e2e/` demonstrates the complete Loom flow from SystemVerilog with DPI calls to a working simulation with host-side function execution.

## What It Tests

1. **DPI transformation**: SystemVerilog DPI-C calls converted to hardware interfaces
2. **Emulation wrapper**: Complete infrastructure generation
3. **Host communication**: Socket-based BFM connecting host to simulation
4. **DPI service**: Generic service loop dispatching calls to user functions

## Test Design

The test design (`dpi_test.sv`) is a simple state machine that:

1. Calls `dpi_add(42, 17)` and stores the result
2. Verifies the result equals 59
3. Calls `dpi_report_result(passed, result)` to report success/failure

```systemverilog
import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function int dpi_report_result(input int passed, input int result);

module dpi_test (
    input  logic clk_i,
    input  logic rst_ni
);
    // State machine: Idle -> CallAdd -> CheckResult -> Report -> Done
    ...
endmodule
```

## Running the Test

```bash
cd tests/e2e
make clean
make run
```

This executes:

1. **Transform** (`make transform`): Yosys with loom plugins
   - `read_slang` parses SystemVerilog
   - `loom_instrument` converts DPI calls, adds flop enable, generates header
   - `emu_top` creates infrastructure wrapper
   - `write_verilog` outputs transformed design

2. **Simulate** (`make sim`): Verilator builds simulation binary
   - Compiles transformed RTL + infrastructure modules
   - Links socket BFM DPI code

3. **Host** (`make host`): GCC builds host binary
   - Compiles test harness with DPI implementations
   - Links libloom and DPI service library

4. **Run** (`make run`): Starts both and connects them
   - Simulation listens on Unix socket
   - Host connects, releases reset, starts emulation
   - DPI service loop handles function calls
   - Test completes when `dpi_report_result` signals exit

## Expected Output

```
=== Starting Loom Simulation ===
[sim] Reset released at t=100000
[loom_bfm] Waiting for connection on /tmp/loom_sim.sock ...
=== Running Host ===
[loom_bfm] Connected
[test] Loom E2E Test Harness
[test] Connected. Design ID: 0xe2e00001, Version: 0x00000100, N_DPI: 2
[test] Starting DPI service loop...
[dpi_service] Entering service loop (n_funcs=2)
[dpi] dpi_add(42, 17) = 59
[dpi] dpi_report_result(passed=1, result=59)
[dpi] TEST PASSED: result=59
[dpi_service] Exit requested by DPI callback
[test] Final cycle count: 9
[test] TEST COMPLETED SUCCESSFULLY
```

## File Structure

```
tests/e2e/
├── dpi_test.sv      # DUT with DPI calls
├── dpi_impl.c       # User DPI function implementations
├── test_main.c      # Test harness using loom_dpi_service
├── Makefile         # Build and run automation
└── build/           # Generated (not committed)
    ├── run.ys               # Yosys script
    ├── transformed_dpi_test.v  # Transformed Verilog
    ├── dpi_test_dpi.h       # Generated C header
    ├── dpi_meta.json        # DPI metadata
    └── obj_dir/             # Verilator output
```

## Implementing DPI Functions

User-implemented functions go in `dpi_impl.c`:

```c
#include "dpi_test_dpi.h"
#include "loom_dpi_service.h"

int32_t dpi_add(int32_t a, int32_t b) {
    return a + b;
}

int32_t dpi_report_result(int32_t passed, int32_t result) {
    printf("Test %s\n", passed ? "PASSED" : "FAILED");
    loom_dpi_service_request_exit();  // Signal completion
    return 0;
}
```

The function names must match exactly what the generated header declares.

## Troubleshooting

**Simulation hangs**: Check that `loom_dpi_service_request_exit()` is called to signal completion.

**Connection refused**: Ensure simulation starts before host (Makefile handles this with `sleep 1`).

**Wrong results**: Check DPI function argument order matches the SV declaration.
