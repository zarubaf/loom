<!-- SPDX-License-Identifier: Apache-2.0 -->
# Emulation Top Wrapper (emu_top)

The `emu_top` Yosys pass generates a complete emulation wrapper around a transformed DUT, providing all the infrastructure needed for host communication.

## Overview

After running `dpi_bridge` on a design, the `emu_top` pass creates a `loom_emu_top` module that:

1. Instantiates the transformed DUT
2. Adds clock gating and reset control
3. Connects DPI interfaces to a register file
4. Provides an AXI-Lite interface for host access

## Pass Usage

```tcl
emu_top [options]

Options:
  -top <module>       DUT module name (required)
  -n_dpi_funcs <N>    Number of DPI functions (default: 0)
```

Example:
```tcl
read_slang design.sv -top my_module
proc
dpi_bridge -json_out dpi_meta.json -header_out my_module_dpi.h
emu_top -top my_module -n_dpi_funcs 2
write_verilog -noattr transformed.v
```

## Generated Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│ loom_emu_top                                                    │
│                                                                 │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐      │
│  │ AXI-Lite     │────│ loom_axi_    │────│ loom_emu_    │      │
│  │ Interface    │    │ interconnect │    │ ctrl         │      │
│  └──────────────┘    └──────┬───────┘    └──────────────┘      │
│                             │                    │              │
│                      ┌──────┴───────┐      emu_clk_en          │
│                      │ loom_dpi_    │            │              │
│                      │ regfile      │            ▼              │
│                      └──────┬───────┘    ┌──────────────┐      │
│                             │            │ loom_clk_gate│      │
│                      ┌──────┴───────┐    └──────┬───────┘      │
│                      │ loom_emu_    │           │              │
│                      │ wrapper      │◄──────────┘              │
│                      └──────┬───────┘                          │
│                             │                                   │
│                      ┌──────┴───────┐                          │
│                      │    DUT       │                          │
│                      │ (transformed)│                          │
│                      └──────────────┘                          │
└─────────────────────────────────────────────────────────────────┘
```

## Infrastructure Modules

### loom_emu_ctrl

Controls emulation state machine and provides status/control registers.

**Register Map (offset from 0x0000):**

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| 0x00 | EMU_STATUS | R | Current state (0=Idle, 1=Running, 2=Frozen) |
| 0x04 | EMU_CONTROL | W | Command (1=Start, 2=Stop, 3=Step) |
| 0x08 | EMU_CYCLE_LO | R | Cycle counter [31:0] |
| 0x0C | EMU_CYCLE_HI | R | Cycle counter [63:32] |
| 0x18 | DUT_RESET_CTRL | W | Bit 0: assert, Bit 1: release |
| 0x20 | N_DPI_FUNCS | R | Number of DPI functions |
| 0x34 | DESIGN_ID | R | Design identifier |
| 0x38 | LOOM_VERSION | R | Toolchain version |

### loom_dpi_regfile

Per-function argument and result registers for DPI calls.

**Per-function register block (64 bytes each, starting at 0x0100):**

| Offset | Name | R/W | Description |
|--------|------|-----|-------------|
| +0x00 | STATUS | R | Bit 0: call pending |
| +0x04 | CONTROL | W | Bit 1: complete call |
| +0x08 | ARG0 | R | Argument 0 |
| +0x0C | ARG1 | R | Argument 1 |
| ... | ARGn | R | Up to 8 arguments |
| +0x28 | RET_LO | W | Return value [31:0] |
| +0x2C | RET_HI | W | Return value [63:32] |

### loom_emu_wrapper

Bridges between the DUT's DPI interface and the regfile:

- Captures DPI calls from the DUT
- Demultiplexes to per-function regfile slots
- Gates the DUT clock during DPI stalls
- Returns results to the DUT

### loom_clk_gate

Generates the gated clock for the DUT based on:
- `emu_clk_en` from the controller
- DPI stall conditions

## Clock Gating

The DUT clock is gated (stopped) when:

1. Emulation is not in Running state
2. A DPI call is pending and waiting for host response

This ensures deterministic behavior - the DUT only advances when all DPI calls have been serviced.

## Ports

The generated `loom_emu_top` has these ports:

| Port | Direction | Description |
|------|-----------|-------------|
| `clk_i` | input | System clock |
| `rst_ni` | input | Active-low reset |
| `s_axil_*` | input/output | AXI-Lite slave interface |
