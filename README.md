<!-- SPDX-License-Identifier: Apache-2.0 -->
<p align="center">
  <img src="./doc/fig/logo_large.jpeg" alt="Loom Logo" width="400">
</p>

<h1 align="center">Loom</h1>

<p align="center">
  <i>Weaving simulation into silicon, one thread at a time.</i>
</p>

Loom is an open-source FPGA emulation toolchain. It transforms
simulation-grade SystemVerilog (including DPI-C function calls) into
FPGA-synthesizable RTL with a host communication interface, enabling
hardware-accelerated verification without rewriting testbenches.

## Features

### DPI-C Function Bridging

DPI-C import calls in your RTL are automatically transformed into a
hardware mailbox interface. At runtime the host executes the original C
functions and returns results to the design, transparently preserving the
DPI contract.

Supported argument types:
- `int`, `shortint`, `longint`, `byte` (signed scalars)
- `bit [N:0]`, `logic [N:0]` (unsigned, arbitrary width)
- `bit [M:0] data[]` (open arrays — input and output via `svOpenArrayHandle`)
- `bit [M:0] data[N]` (fixed-size arrays)
- `string` (compile-time constant)

Supported return types: `void`, `int`, `shortint`, `longint`, `byte`,
sized `bit`/`logic`.

Open arrays are fully SVDPI-compatible — user C code uses standard
`svGetArrayPtr()`, `svLength()`, etc. The same DPI function can be called
from multiple modules with different array sizes; Loom infers the element
count from each call site's local variable. Compatible with libraries like
[multisim](https://github.com/antoinemadec/multisim).

DPI calls in `initial` blocks (void, side-effect only) and reset blocks
(providing DPI-computed initial register values) are also supported —
they execute on the host before the design starts running.

### `$display` / `$finish`

`$display` and `$write` calls are bridged to `printf` on the host.
`$finish` and `$fatal` trigger clean emulation shutdown with an exit code.

### State Capture and Restore

All flip-flops in the design are chained into a scan chain. The host can
capture a full snapshot of the design state and restore it later. Snapshots
are serialized as protobuf and include symbolic variable names from the
original HDL hierarchy.

### Memory Shadow Ports

BRAM memories can be accessed directly from the host via shadow read/write
ports, without having to shift through the scan chain. This is much faster
for large memories.

### Reset Extraction

Asynchronous resets are stripped from the design and replaced with
scan-based initialization. Reset values are extracted from the RTL and
applied via the scan chain at startup, eliminating the reset distribution
tree from the synthesized design.

### Interactive Shell

`loomx` provides a REPL with tab completion, history, and the following
commands:

| Command            | Alias | Description                                                                |
| ------------------ | ----- | -------------------------------------------------------------------------- |
| `run [N]`          | `r`   | Release reset and start emulation; service DPI calls. Ctrl+C to interrupt. |
| `stop`             |       | Freeze emulation, preserving state.                                        |
| `step [N]`         | `s`   | Advance N clock cycles (default 1).                                        |
| `status`           | `st`  | Print state, cycle count, design info, DPI statistics.                     |
| `read <addr>`      |       | Read a 32-bit register at the given hex address.                           |
| `write <a> <d>`    | `wr`  | Write 32-bit hex value to the given hex address.                           |
| `dump [file.pb]`   | `d`   | Capture and display scan chain contents. Optionally save to protobuf.      |
| `inspect <file.pb>`|       | Load and display a saved snapshot.                                         |
| `deposit_script`   |       | Generate `$deposit` SystemVerilog from a snapshot file.                    |
| `reset`            |       | Assert DUT reset.                                                          |
| `couple`           |       | Clear decoupler — connect emu_top to AXI bus.                              |
| `decouple`         |       | Assert decoupler — isolate emu_top (transactions return SLVERR).           |
| `exit`             | `q`   | Disconnect and exit.                                                       |

### FPGA Target

Loom supports running on Alveo U250 FPGAs over PCIe (XDMA). A single
`loom_shell` top-level module is shared between simulation and FPGA —
only the sub-module implementations differ (behavioral BFMs for simulation,
Xilinx IPs for FPGA). See [doc/fpga-support.md](doc/fpga-support.md).

## Prerequisites

### macOS

```bash
brew install pkg-config libffi bison readline autoconf
```

### Linux (Debian/Ubuntu)

```bash
sudo apt-get install build-essential cmake bison flex libfl-dev \
    pkg-config libffi-dev libreadline-dev zlib1g-dev tcl-dev \
    autoconf ccache help2man perl python3 git
```

Yosys, yosys-slang, and Verilator are fetched and built automatically.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This builds:
- **Yosys** (fetched automatically) with the Loom transformation passes
- **`loomc`** — the compilation driver
- **`loomx`** — the execution host and interactive shell

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
-top MODULE      Top module name (required)
-work DIR        Work/output directory (default: work/)
-f FILELIST      Read source files from filelist
-clk SIGNAL      Clock signal name (default: clk_i)
-rst SIGNAL      Reset signal name (default: rst_ni)
-D DEFINE        Preprocessor define (passed to slang)
--mem-shadow     Enable memory shadow ports
-v               Verbose output
```

This produces a work directory containing:
```
build/
  transformed.v            # FPGA-synthesizable Verilog
  loom_dpi_dispatch.so     # Compiled dispatch table
  scan_map.pb              # Scan chain map (protobuf)
```

### 2. Compile User DPI Code

Compile your DPI function implementations into a shared object.
Include `src/include` for `svdpi.h` if your code uses open arrays:

```bash
cc -shared -fPIC -I$LOOM_HOME/src/include -o build/libdpi.so dpi_impl.c
```

### 3. Build the Verilator Simulation

Build a Verilator binary from the transformed Verilog and Loom
infrastructure RTL:

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
-timeout NS     Simulation timeout in ns
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

## How It Works

```
                         loomc                                  loomx
   ┌──────────┐      ┌────────────┐      ┌───────────┐      ┌──────────┐
   │  DUT.sv  │─────▶│   Yosys    │─────▶│ Verilator │─────▶│   Host   │
   │ (DPI-C)  │      │  + passes  │      │   sim     │      │  shell   │
   └──────────┘      └────────────┘      └───────────┘      └──────────┘
                      reset_extract            ▲                  │
                      scan_insert              │    Unix socket   │
                      loom_instrument    ┌─────┴───────┐          │
                      emu_top            │  AXI-Lite   │◀─────────┘
                           │             │    BFM      │  DPI dispatch
                           ▼             └─────────────┘  + user .so
                      transformed.v
                      dispatch.so
```

1. **`loomc`** runs Yosys with Loom passes to extract reset values, insert
   scan chains, transform DPI calls into a hardware mailbox interface, and
   generate the emulation wrapper. It also compiles the generated dispatch
   table.

2. The user builds a **Verilator simulation** from the transformed Verilog
   and Loom infrastructure RTL.

3. **`loomx`** loads the dispatch and user DPI shared objects, launches the
   simulation, connects via a Unix domain socket through the AXI-Lite BFM,
   and services DPI calls in real time.

## Missing Features

- [ ] Arbitrary IO signals on top level of DUT
- [ ] Synthesizable assertions
- [ ] DPI export
- [ ] Faster read-only DPI (for state streaming off-chip)
- [ ] Automatic design partitioning and multi-fpga
- [ ] Time multiplex multiple instantiated designs
- [ ] Full end-to-end depositing with `verilator` (or any other simulator)
- [ ] Run arbitrary SystemVerilog in automatically partitioned `verilator` <->
  `loom` interaction (would allow for UVM and other stuff)
- [ ] Deterministic DDR interface for DUTs that need large memory
- [ ] Re-calculating sim time from clock period
- [ ] More than one clock (some fixed ratio design seems the most
  straight-forward)
- [ ] Annotate bram or ultram onto memories. brams are dual-ported, the shadow
  ports could go directly there (-> less resource utilization).

## License

Apache-2.0. See [LICENSE](LICENSE).
