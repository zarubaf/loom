<!-- SPDX-License-Identifier: Apache-2.0 -->
![Loom Logo](doc/fig/logo.png)

# Loom

Loom is an open-source FPGA emulation toolchain. It transforms
simulation-grade SystemVerilog (including DPI-C function calls) into
FPGA-synthesizable RTL with a host communication interface, enabling
hardware-accelerated verification without rewriting testbenches.

## Features

**DPI-C Function Bridging** - DPI-C import calls in your RTL are
automatically transformed into a hardware mailbox interface. At runtime the
host executes the original C functions and returns results to the design,
transparently preserving the DPI contract.

Supported argument types:
- `int`, `shortint`, `longint`, `byte` (signed)
- `bit [N:0]`, `logic [N:0]`, `reg [N:0]` (unsigned, arbitrary width)
- `string` (compile-time constant)

Supported return types: `void`, `int`, `shortint`, `longint`, `byte`,
sized `bit`/`logic`.

**`$display` / `$write` Support** - `$display` calls in `always_ff`
blocks are converted to DPI calls automatically. Format strings (`%x`,
`%d`, `%u`, `%o`, `%s`) are executed on the host, printing to stdout just
like a normal simulation.

**`$finish` Support** - `$finish` statements are bridged to the host
so the simulation terminates cleanly when the design signals completion.

**Interactive Shell** - `loomx` provides a REPL with tab completion,
history, and the following commands:

| Command    | Description                                                                |
| ---------- | -------------------------------------------------------------------------- |
| `run [N]`  | Release reset and start emulation; service DPI calls. Ctrl+C to interrupt. |
| `stop`     | Freeze emulation, preserving state.                                        |
| `step [N]` | Advance N clock cycles (default 1).                                        |
| `status`   | Print state, cycle count, design info, DPI statistics.                     |
| `dump`     | Capture and display scan chain contents.                                   |
| `reset`    | Assert DUT reset.                                                          |
| `exit`     | Disconnect and exit.                                                       |

## Prerequisites

### macOS

```bash
brew install pkg-config libffi bison readline verilator
```

### Linux (Debian/Ubuntu)

```bash
apt install build-essential cmake pkg-config libffi-dev libreadline-dev verilator
```

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This builds:
- **Yosys** (fetched automatically) with the Loom transformation passes
- **`loomc`** - the compilation driver
- **`loomx`** - the execution host

### Running Tests

```bash
ctest --test-dir build --output-on-failure
```

### Installing (optional)

```bash
cmake --install build --prefix ~/.local
```

## Usage

The workflow has three steps: **compile**, **build sim**, **execute**.

### 1. Compile with `loomc`

`loomc` runs the Yosys transformation pipeline and compiles the DPI
dispatch table into a shared object.

```bash
loomc -top my_dut -work build/ my_dut.sv
```

Options:
```
-top MODULE    Top module name (required)
-work DIR      Work/output directory (default: work/)
-f FILELIST    Read source files from filelist
-clk SIGNAL    Clock signal name (default: clk_i)
-rst SIGNAL    Reset signal name (default: rst_ni)
-v             Verbose output
```

This produces a work directory containing:
```
build/
  transformed.v            # FPGA-synthesizable Verilog
  loom_dpi_dispatch.so     # Compiled dispatch table
  dpi_meta.json            # DPI metadata
  scan_map.json            # Scan chain map
```

### 2. Compile User DPI Code

Compile your DPI function implementations into a shared object.
No Loom headers are needed - just your own code:

```bash
cc -shared -fPIC -o libdpi.so dpi_impl.c
```

### 3. Build the Verilator Simulation

Build a Verilator binary from the transformed Verilog and Loom
infrastructure RTL. The included `src/util/mk/loom_sim.mk` provides a pattern
rule for this (see [Test Integration](#test-integration) below), or
build manually:

```bash
verilator --binary --timing \
    src/rtl/loom_emu_ctrl.sv \
    src/rtl/loom_axi_interconnect.sv \
    src/rtl/loom_dpi_regfile.sv \
    src/rtl/loom_scan_ctrl.sv \
    src/bfm/loom_axil_socket_bfm.sv \
    src/test/loom_sim_top.sv \
    build/transformed.v \
    src/bfm/loom_sock_dpi.c \
    --top-module loom_sim_top \
    --Mdir build/sim/obj_dir -o Vloom_sim_top
```

### 4. Execute with `loomx`

`loomx` loads the dispatch and user shared objects, launches the
simulation, and provides an interactive shell (or runs a script):

```bash
# Interactive
loomx -work build/ -sv_lib dpi -sim Vloom_sim_top

# Scripted
loomx -work build/ -sv_lib dpi -sim Vloom_sim_top -f test_script.txt
```

Options:
```
-work DIR       Work directory from loomc (required)
-sv_lib NAME    User DPI shared library (without lib prefix / .so suffix)
-sim BINARY     Simulation binary name (default: Vloom_sim_top)
-f SCRIPT       Run commands from script file
-s SOCKET       Socket path (default: auto PID-based)
--no-sim        Don't launch sim (connect to existing)
-v              Verbose output
```

## Example

The `tests/e2e/` directory contains a complete working example:

- **`dpi_test.sv`** - an LFSR-based test module that calls `dpi_add`
  eight times and verifies results, uses `$display` for output, and calls
  `$finish` when done.
- **`dpi_impl.c`** - pure C implementations of `dpi_add` and
  `dpi_report_result` (no Loom headers needed).
- **`Makefile`** - ~25-line Makefile using `loom.mk` / `loom_sim.mk`.

Run it:

```bash
cd tests/e2e
LOOM_HOME=../.. make test
```

## How It Works

```
                         loomc                                  loomx
   ┌──────────┐      ┌────────────┐      ┌───────────┐      ┌──────────┐
   │  DUT.sv  │─────▶│   Yosys    │─────▶│ Verilator │─────▶│   Host   │
   │ (DPI-C)  │      │  + passes  │      │   sim     │      │  shell   │
   └──────────┘      └────────────┘      └───────────┘      └──────────┘
                      scan_insert              ▲                  │
                      loom_instrument          │    Unix socket   │
                      emu_top            ┌─────┴───────┐          │
                           │             │  AXI-Lite   │◀─────────┘
                           ▼             │    BFM      │  DPI dispatch
                      transformed.v      └─────────────┘  + user .so
                      dispatch.so
```

1. **`loomc`** runs Yosys with Loom passes to transform DPI calls into a
   hardware mailbox interface, insert scan chains, and generate the
   emulation wrapper. It also compiles the generated dispatch table.

2. The user builds a **Verilator simulation** from the transformed Verilog
   and Loom infrastructure RTL.

3. **`loomx`** loads the dispatch and user DPI shared objects, launches the
   simulation, connects via a Unix domain socket through the AXI-Lite BFM,
   and services DPI calls in real time.
