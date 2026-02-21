<!-- SPDX-License-Identifier: Apache-2.0 -->
# CLAUDE.md — Loom: Open-Source FPGA Emulation Toolchain

## Quick Start

```bash
# Prerequisites (macOS)
brew install pkg-config libffi bison readline autoconf

# Prerequisites (Ubuntu)
sudo apt-get install build-essential cmake bison flex libfl-dev \
    pkg-config libffi-dev libreadline-dev zlib1g-dev tcl-dev \
    autoconf ccache help2man perl python3 git

# Build (automatically fetches and builds Yosys, yosys-slang, and Verilator v5.044)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure
```

## Project Overview

Loom transforms simulation-grade SystemVerilog (including DPI-C calls, multi-cycle timing blocks, `$display`, `$finish`) into FPGA-synthesizable RTL with host communication infrastructure.

**Pipeline:**
```
Source SV → yosys-slang (--loom, FSM extraction) → Loom passes → Vivado/Verilator
```

**Transformation passes (required order):**
1. `mem_shadow` — Shadow read/write ports for memory access (before flatten, no-op without memories)
2. `reset_extract` — Strip async resets, record initial values as wire attributes
3. `loom_instrument` — DPI bridge, FF enable (`loom_en`), `$finish` → `loom_finish_o`
4. `scan_insert` — Scan chain insertion, protobuf state map with reset values
5. `emu_top` — Emulation wrapper with AXI-Lite interface, IRQ, DPI regfile

**Frontend (yosys-slang `--loom`):**
- FSM extraction: multi-cycle `always` blocks with inner `@(posedge clk)`, `while-wait`, `repeat` → state machines
- DPI detection: `$__loom_dpi_call` cells for DPI imports
- `$finish`/`$display` detection: `$__loom_finish` / `$print` cells
- `$readmemh`/`$readmemb` metadata capture for runtime memory loading

**Other key components:**
- `passes/mem_shadow/` - Shadow read/write ports for BRAM access (always runs, before flatten)
- `src/host/` - Host-side C++ library (interrupt-driven DPI service, socket + XDMA transports)
- `src/rtl/` - Infrastructure RTL (emu_ctrl, dpi_regfile, scan_ctrl, shell)
- `src/bfm/` - Behavioral models for sim (socket BFM, clock gen, CDC, decoupler)
- `src/tools/` - `loomc` (compiler) and `loomx` (execution host)

## Documentation

Design documentation lives in `doc/`:
- `doc/pass-pipeline.md` - **Transformation pass pipeline** (all passes, FSM extraction, data flow)
- `doc/dpi-bridge-internals.md` - DPI bridge implementation details
- `doc/emu-top.md` - Emulation wrapper architecture and register map
- `doc/host-library.md` - Host-side libloom, loomx shell, interrupt-driven DPI service, transports
- `doc/e2e-test.md` - End-to-end test guide
- `doc/fpga-support.md` - FPGA build flow and architecture
- `doc/shutdown-spec.md` - Clean shutdown mechanism ($finish, host finish)

**Keep documentation up-to-date.** When making significant changes to a component, update the corresponding documentation in `doc/`.

## Coding Conventions

### Yosys/C++
- Use `log()` / `log_error()`, never `printf`
- Use `NEW_ID` for auto-generated names, `ID(\name)` for user-visible
- Always call `module->fixup_ports()` after modifying ports
- Wrap pass internals in `PRIVATE_NAMESPACE_BEGIN` / `PRIVATE_NAMESPACE_END`

### SystemVerilog (lowRISC style)
- Reset: active-low `rst_ni`
- Clock: `clk_i`
- Ports: `_i` suffix for inputs, `_o` for outputs
- Registered signals: `_d` for next-state, `_q` for registered
- State enums: `StIdle`, `StActive`, etc.

### Signal Naming

All Loom-generated signals use the `loom_` prefix:

| Pass | Signals / Attributes |
|------|---------------------|
| `reset_extract` | `loom_reset_value` (wire attr), `loom_resets_extracted` (module attr) |
| `loom_instrument` | `loom_en`, `loom_dpi_valid`, `loom_dpi_func_id`, `loom_dpi_args`, `loom_dpi_result`, `loom_finish_o` |
| `scan_insert` | `loom_scan_enable`, `loom_scan_in`, `loom_scan_out`, `loom_scan_chain_length` (module attr) |
| `mem_shadow` | `loom_shadow_addr`, `loom_shadow_rdata/wdata/wen/ren`, `loom_n_memories` (module attr) |

## Files to Never Commit

Only source files should be committed:

- `*.log` - Build/Yosys logs
- `*.vcd` - Waveform dumps
- `*.json` - Generated metadata (e.g., `dpi_meta.json`)
- `transformed*.v` - Generated Verilog
- `build/` - Build directory
- `*_dpi.h` - Generated headers (in test directories)

## FPGA Build (Alveo U250)

```bash
# Prerequisites: Vivado 2024.1
export PATH=/opt/eda/ubuntu-24.04/xilinx/Vivado/2024.1/bin:$PATH

# Generate Xilinx IPs (one-time)
make -C fpga ip

# Build bitstream (requires transformed DUT)
make -C fpga bitstream TRANSFORMED_V=path/to/transformed.v

# Program FPGA via JTAG
make -C fpga program
```

**Architecture:**
- `src/rtl/loom_shell.sv` — Unified top-level for sim + FPGA (XDMA, demux, decoupler, CDC, clk_gen, emu_top)
- `src/rtl/loom_axil_demux.sv` — Shared parameterizable AXI-Lite 1:N demux
- `src/bfm/` — Behavioral models (xlnx_xdma, xlnx_clk_gen, xlnx_cdc, xlnx_decoupler, xilinx_primitives)
- `fpga/` — Vivado scripts, IP generation, board constraints
- `src/host/loom_transport_xdma.cpp` — PCIe/XDMA host transport

See `doc/fpga-support.md` for detailed architecture documentation.

## Implementation Notes

**No clock gating:** DUT clock runs free (ungated). State freezing is via the `loom_en` FF enable signal: `loom_en = emu_running & (!dpi_valid | dpi_ack)`. Scan enable overrides: `EN_eff = (loom_en & orig_EN) | loom_scan_enable`.

**Scan-based initialization:** Hardware reset is stripped by `reset_extract`. Initial state is loaded via the scan chain before the first emulation run. The host reads reset values from `scan_map.pb` and scans them in.

**Interrupt-driven DPI:** `irq_o[0] = |dpi_stall` wakes the host when any DPI call is pending. The host reads a single DPI pending mask register (`0x1_FFC0`) to determine which functions need servicing. Falls back to 1ms polling when the transport lacks interrupt support.

**Plugin linking on macOS:** Yosys plugins must NOT link against libyosys.so directly. Use `-undefined dynamic_lookup` to resolve symbols at runtime. See `passes/CMakeLists.txt`.

**Required compile definitions for plugins:**
- `_YOSYS_`
- `YOSYS_ENABLE_PLUGINS`
- `YOSYS_ENABLE_GLOB`
- `YOSYS_ENABLE_ZLIB`
