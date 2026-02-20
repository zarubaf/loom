<!-- SPDX-License-Identifier: Apache-2.0 -->
![Loom Logo](doc/fig/logo.png)

# Loom

Loom is an open-source FPGA emulation toolchain. It transforms
simulation-grade SystemVerilog (including DPI-C function calls) into
FPGA-synthesizable RTL with a host communication interface, enabling
hardware-accelerated verification without rewriting testbenches.

## Features

**DPI-C Function Bridging** — DPI-C import calls in your RTL are
automatically transformed into a hardware mailbox interface. At runtime the
host executes the original C functions and returns results to the design,
transparently preserving the DPI contract.

Supported argument types:
- `int`, `shortint`, `longint`, `byte` (signed scalars)
- `bit [N:0]`, `logic [N:0]` (unsigned, arbitrary width)
- `bit [M:0] data[]` (open arrays — input and output via `svOpenArrayHandle`)
- `string` (compile-time constant)

Supported return types: `void`, `int`, `shortint`, `longint`, `byte`,
sized `bit`/`logic`.

Open arrays are fully SVDPI-compatible — user C code uses standard
`svGetArrayPtr()`, `svLength()`, etc. The same DPI function can be called
from multiple modules with different array sizes; Loom infers the element
count from each call site's local variable. Compatible with libraries like
[multisim](https://github.com/antoinemadec/multisim). 4-state and 2-state
are treated identically (no X/Z on FPGA).

**`$display` / `$finish`** — `$display` calls are bridged to `printf`
on the host. `$finish` triggers clean simulation shutdown.

**Interactive Shell** - `loomx` provides a REPL with tab completion,
history, and the following commands:

| Command           | Description                                                                |
| ----------------- | -------------------------------------------------------------------------- |
| `run [N]`         | Release reset and start emulation; service DPI calls. Ctrl+C to interrupt. |
| `stop`            | Freeze emulation, preserving state.                                        |
| `step [N]`        | Advance N clock cycles (default 1).                                        |
| `status`          | Print state, cycle count, design info, DPI statistics.                     |
| `read <addr>`     | Read a 32-bit register at the given hex address.                           |
| `write <a> <d>`   | Write 32-bit hex value to the given hex address. Alias: `wr`.              |
| `dump`            | Capture and display scan chain contents.                                   |
| `reset`           | Assert DUT reset.                                                          |
| `couple`          | Clear decoupler — connect emu_top to AXI bus.                              |
| `decouple`        | Assert decoupler — isolate emu_top (transactions return SLVERR).           |
| `exit`            | Disconnect and exit.                                                       |

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

The workflow has four steps: **compile**, **build DPI**, **build sim**, **execute**.

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
Include `src/include` for `svdpi.h` if your code uses open arrays:

```bash
cc -shared -fPIC -I$LOOM_HOME/src/include -o build/libdpi.so dpi_impl.c
```

### 3. Build the Verilator Simulation

Build a Verilator binary from the transformed Verilog and Loom
infrastructure RTL. The included `src/util/mk/loom_sim.mk` provides a pattern
rule for this (see [Test Integration](#test-integration) below), or
build manually:

```bash
verilator --binary --timing \
    src/rtl/loom_emu_ctrl.sv \
    src/rtl/loom_axil_demux.sv \
    src/rtl/loom_dpi_regfile.sv \
    src/rtl/loom_scan_ctrl.sv \
    src/rtl/loom_shell.sv \
    src/bfm/loom_axil_socket_bfm.sv \
    src/bfm/xlnx_xdma.sv \
    src/bfm/xlnx_clk_gen.sv \
    src/bfm/xlnx_cdc.sv \
    src/bfm/xlnx_decoupler.sv \
    src/bfm/xilinx_primitives.sv \
    build/transformed.v \
    src/bfm/loom_sock_dpi.c \
    --top-module loom_shell \
    --Mdir build/sim/obj_dir -o Vloom_shell
```

### 4. Execute with `loomx`

`loomx` loads the dispatch and user shared objects, launches the
simulation, and provides an interactive shell (or runs a script):

```bash
# Interactive
loomx -work build/ -sv_lib dpi -sim Vloom_shell

# Scripted
loomx -work build/ -sv_lib dpi -sim Vloom_shell -f test_script.txt
```

Options:
```
-work DIR       Work directory from loomc (required)
-sv_lib NAME    User DPI shared library (without lib prefix / .so suffix)
-sim BINARY     Simulation binary name (default: Vloom_shell)
-f SCRIPT       Run commands from script file
-s SOCKET       Socket path (default: auto PID-based)
-t TRANSPORT    Transport: socket (default) or xdma
-d DEVICE       XDMA device path or PCI BDF (default: /dev/xdma0_user)
--no-sim        Don't launch sim (connect to existing)
-v              Verbose output
```

#### XDMA mode (FPGA)

When using `-t xdma`, `loomx` connects directly to an FPGA over PCIe
instead of launching a simulation:

```bash
loomx -work build/ -t xdma                        # default /dev/xdma0_user
loomx -work build/ -t xdma -d 0000:17:00.0        # PCI BDF (mmap BAR0)
loomx -work build/ -t xdma -f fpga_script.txt      # scripted FPGA control
```

The `-work` directory is always required (for loading the DPI dispatch table).
The `-sim` flag cannot be combined with `-t xdma`.

## Examples

### Scalar DPI (`tests/e2e/`)

An LFSR-based test that calls `dpi_add` eight times, verifies results
via `$display`, and exits with `$finish`:

```bash
cd tests/e2e && LOOM_HOME=../.. make test
```

### Open array DPI (`tests/dpi_open_array/`)

Exercises input and output open arrays — fills an array via
`dpi_fill_array`, reads it back via `dpi_sum_array`, and verifies
the round-trip:

```bash
cd tests/dpi_open_array && LOOM_HOME=../.. make test
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
