<!-- SPDX-License-Identifier: Apache-2.0 -->
# End-to-End Test

The e2e test in `tests/e2e/` demonstrates the complete Loom flow from
SystemVerilog with DPI calls to a working simulation with host-side
function execution.

## What It Tests

1. **DPI transformation**: SystemVerilog DPI-C calls converted to hardware interfaces
2. **Emulation wrapper**: Complete infrastructure generation
3. **Host communication**: Socket-based BFM connecting host to simulation
4. **DPI service**: Generic service loop dispatching calls to user functions
5. **`$display` bridging**: Format strings forwarded to host `printf`
6. **`$finish` shutdown**: Clean exit triggered by the DUT

## Test Design

The test design (`dpi_test.sv`) is a state machine that:

1. Uses an LFSR to generate pseudo-random operands
2. Calls `dpi_add(a, b)` eight times and verifies each result
3. Calls `dpi_report_result(passed, failed)` to report success/failure
4. Exits with `$finish`

```systemverilog
import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function int dpi_report_result(input int passed, input int result);

module dpi_test (
    input  logic clk_i,
    input  logic rst_ni
);
    // State machine: Idle -> CallAdd -> Check -> Next -> Report -> Done ($finish)
    ...
endmodule
```

## Running the Test

From the build directory (after building Loom):

```bash
ctest --test-dir build -R e2e_dpi_test --output-on-failure
```

Or manually from the test directory:

```bash
cd tests/e2e
make test
```

## What `make test` Does

The `Makefile` uses the shared `loom_test.mk` recipe, which runs four
steps:

1. **`loomc`** transforms the DUT (scan insertion, DPI bridging, emulation
   wrapper) and compiles the DPI dispatch table.

2. **Verilator** builds a simulation binary from the transformed Verilog
   and Loom infrastructure RTL.

3. **User DPI code** (`dpi_impl.c`) is compiled into `libdpi.so`.

4. **`loomx`** launches the simulation, loads both `loom_dpi_dispatch.so`
   and `libdpi.so`, connects via Unix socket, and runs the design. DPI
   calls are serviced automatically. The design hits `$finish` and `loomx`
   exits cleanly.

## File Structure

```
tests/e2e/
├── dpi_test.sv      # DUT with DPI calls
├── dpi_impl.c       # User DPI function implementations
├── Makefile         # TOP, DUT_SRC, DPI_SRCS + include loom_test.mk
└── build/           # Generated (not committed)
    ├── transformed.v        # Transformed Verilog
    ├── loom_dpi_dispatch.so # Compiled dispatch table
    ├── scan_map.pb          # Scan chain map
    ├── libdpi.so            # User DPI shared library
    └── sim/obj_dir/         # Verilator output
```

## Writing Your Own E2E Test

Create a directory with three files:

**`Makefile`:**
```make
TOP      := my_dut
DUT_SRC  := my_dut.sv
DPI_SRCS := dpi_impl.c

include path/to/loom_test.mk
```

**`my_dut.sv`** — your DUT with DPI imports (calls must be in
`always_ff` blocks).

**`dpi_impl.c`** — your DPI function implementations. These are pure C
functions with no Loom-specific includes needed:

```c
#include <stdio.h>
#include <stdint.h>

int32_t dpi_add(int32_t a, int32_t b) {
    return a + b;
}
```

Then run:
```bash
make test          # scripted (run + exit)
make interactive   # interactive shell
make clean         # remove build artifacts
```

The shared `loom_test.mk` provides these targets and handles the full
`loomc` / Verilator / `loomx` pipeline automatically. It derives
`LOOM_ROOT` from its own location, so `LOOM_HOME` does not need to be set
if the test is inside the Loom source tree.

## Troubleshooting

**Simulation hangs**: The DUT must hit `$finish` to trigger clean shutdown.
If your design runs forever, use `loomx -f script.txt` with a script that
runs for a bounded number of cycles then exits.

**Connection refused**: The socket is created automatically by `loomx`.
If using `--no-sim`, ensure the simulation is running before connecting.

**Wrong results**: Check that DPI function argument order matches the
SystemVerilog declaration and that the C types align (e.g. `int32_t` for
`int`).
