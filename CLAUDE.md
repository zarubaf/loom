<!-- SPDX-License-Identifier: Apache-2.0 -->
# CLAUDE.md — Loom: Open-Source FPGA Emulation Toolchain

## Quick Start

```bash
# Prerequisites (macOS)
brew install pkg-config libffi bison readline

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure
```

## Project Overview

Loom transforms simulation-grade SystemVerilog (including DPI-C calls) into FPGA-synthesizable RTL with host communication infrastructure.

**Pipeline:**
```
Source SV → yosys + loom plugins → transformed Verilog → Vivado/Verilator
```

**Key components:**
- `passes/loom_instrument/` - DPI bridge + flop enable instrumentation
- `passes/emu_top/` - Generates emulation wrapper with AXI-Lite interface
- `src/host/` - Host-side library for communication and DPI service
- `src/rtl/` - Infrastructure RTL modules

## Documentation

Design documentation lives in `doc/`:
- `doc/dpi-bridge-internals.md` - DPI bridge implementation details
- `doc/emu-top.md` - Emulation wrapper architecture
- `doc/host-library.md` - Host-side libloom and DPI service
- `doc/e2e-test.md` - End-to-end test guide

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

| Pass | Signals |
|------|---------|
| `loom_instrument` | `loom_en`, `loom_dpi_valid`, `loom_dpi_ready`, `loom_dpi_func_id`, `loom_dpi_args`, `loom_dpi_result` |
| `scan_insert` | `loom_scan_enable`, `loom_scan_in`, `loom_scan_out` |

## Files to Never Commit

Only source files should be committed:

- `*.log` - Build/Yosys logs
- `*.vcd` - Waveform dumps
- `*.json` - Generated metadata (e.g., `dpi_meta.json`)
- `transformed*.v` - Generated Verilog
- `build/` - Build directory
- `*_dpi.h` - Generated headers (in test directories)

## Implementation Notes

**Plugin linking on macOS:** Yosys plugins must NOT link against libyosys.so directly. Use `-undefined dynamic_lookup` to resolve symbols at runtime. See `passes/CMakeLists.txt`.

**Required compile definitions for plugins:**
- `_YOSYS_`
- `YOSYS_ENABLE_PLUGINS`
- `YOSYS_ENABLE_GLOB`
- `YOSYS_ENABLE_ZLIB`
