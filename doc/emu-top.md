<!-- SPDX-License-Identifier: Apache-2.0 -->
# Emulation Top Wrapper (emu_top)

The `emu_top` Yosys pass generates a complete emulation wrapper around a transformed DUT, providing all the infrastructure needed for host communication.

## Overview

After running `loom_instrument` on a design, the `emu_top` pass creates a `loom_emu_top` module that:

1. Instantiates the transformed DUT
2. Controls DUT state via `loom_en` flip-flop enable
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
scan_insert
loom_instrument -json_out dpi_meta.json -header_out my_module_dpi.h
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
│                      ┌──────┴───────┐       emu_clk_en         │
│                      │ loom_dpi_    │            │              │
│                      │ regfile      │            ▼              │
│                      └──────┬───────┘     loom_en_wire         │
│                      ┌──────┴───────┐    (emu_clk_en &         │
│                      │ loom_emu_    │     !dpi_valid|ack)       │
│                      │ wrapper      │            │              │
│                      └──────┬───────┘            │              │
│                             │                    │              │
│                      ┌──────┴───────┐            │              │
│                      │    DUT       │◄───────────┘              │
│                      │ (transformed)│  loom_en (FF enable)      │
│                      └──────────────┘                           │
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
- Reports DPI stalls to emu_ctrl
- Returns results to the DUT

### loom_scan_ctrl

Controls scan chain capture/restore operations. With free-running clock and FF enable override (`loom_en | loom_scan_enable`), the scan controller simply asserts `scan_enable` and shifts one bit per cycle.

## FF Enable (loom_en)

The DUT clock runs free (ungated). State freezing is done via the `loom_en` flip-flop enable signal:

```
loom_en = emu_clk_en & (!dpi_valid | dpi_ack)
```

DUT FFs are frozen (loom_en=0) when:

1. Emulation is not in Running state (emu_clk_en=0)
2. A DPI call is pending and waiting for host response (dpi_valid=1, dpi_ack=0)

Inside the DUT, each FF's enable is: `loom_en | loom_scan_enable`, so scan operations always work regardless of loom_en.

## Ports

The generated `loom_emu_top` has these ports:

| Port | Direction | Description |
|------|-----------|-------------|
| `clk_i` | input | System clock |
| `rst_ni` | input | Active-low reset |
| `s_axil_*` | input/output | AXI-Lite slave interface |
