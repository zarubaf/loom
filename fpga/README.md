# Loom FPGA Build — Alveo U250

This directory contains the Vivado build flow for running Loom on an Alveo U250
via PCIe/XDMA. It uses **DFX (Dynamic Function eXchange)** so the FPGA
infrastructure (PCIe, clocking, AXI bus) persists on the board across DUT
changes, and only the compiled DUT is swapped.

---

## Glossary

**Bitstream (`.bit`)**
The binary file that configures the FPGA fabric. A *full* bitstream configures
the entire device. A *partial* bitstream configures only one region (the RP).

**DFX — Dynamic Function eXchange**
Xilinx/AMD's name for partial reconfiguration. A portion of the FPGA (the RP)
can be reconfigured at runtime while the rest (the static region) keeps running.
The PCIe link stays up, the AXI bus stays live — only the DUT logic changes.

**Static region**
The part of the FPGA that never changes after the shell is programmed. Contains:
XDMA (PCIe), AXI-Lite demux, firewall, DFX decoupler, CDC, clock generator, and
reset synchroniser. Defined by everything in `loom_shell` except `u_emu_top`.

**RP — Reconfigurable Partition**
The region of the FPGA fabric reserved for swappable logic. In Loom the RP is
the `u_emu_top` cell (122/128 clock regions across all 4 SLRs (~95% of device) on the U250). The RP boundary is fixed at
first-build time and cannot move without rebuilding the static region.

**RM — Reconfigurable Module**
One specific implementation that fills the RP. Every compiled DUT produces one
RM. Multiple RMs can exist on disk; only one is loaded into the RP at a time.
Named via `RM_NAME=` on the make command line (default: `rm`).

**Partial bitstream (`*_partial.bit`)**
A bitstream that reconfigures only the RP. Much smaller than a full bitstream
(covers only 122/128 clock regions across all 4 SLRs (~95% of device)). Loaded via JTAG without disturbing the static region
or the PCIe link. Every new DUT build produces one of these.

**Full bitstream (`full.bit`)**
A bitstream that configures the entire device: static region + one initial RM.
Used only for the first-time flash programming. After that, only partial
bitstreams are needed for DUT swaps.

**MCS (`.mcs`)**
Intel HEX-like file format for SPI flash programming. Generated from `full.bit`
by `write_cfgmem`. Written to the on-board SPI flash by `dfx-program-flash` so
the shell loads automatically on every power-on.

**SPI flash**
The Micron MT25QU01G (1 Gb) NOR flash soldered on the U250 board. Stores the
full bitstream. On power-on the FPGA configuration engine reads from it and
configures the static region + initial RM automatically (~100 ms). Partial
bitstreams are never written to flash — they are loaded directly into FPGA SRAM
via JTAG and lost on power cycle.

**DCP — Design Checkpoint**
Vivado's snapshot format. Stores netlist + constraints + placement + routing.
Three DCPs matter here:

| File | When produced | Contains |
|------|--------------|----------|
| `static_synth.dcp` | `dfx-static` step 1 | Synthesised static netlist with RP as black box |
| `static_routed.dcp` | `dfx-static` step 3 | **Golden checkpoint** — fully placed & routed static region, RP area locked |
| `${RM_NAME}_synth.dcp` | `dfx-rm` step 1 | Synthesised RM netlist (OOC) |

**OOC — Out-of-Context synthesis**
Synthesising a module in isolation without its parent. Used for the RM so
Vivado doesn't need to re-synthesise the whole design for each new DUT.
Boundary ports are treated as top-level I/Os for timing purposes.

**Pblock**
A rectangular region of FPGA fabric assigned to the RP. Defined in
`boards/u250/u250_dfx.xdc`. Must enclose all LUTs, FFs, BRAMs, and DSPs of
`u_emu_top`. Fixed at first-build time — changing the pblock invalidates
`static_routed.dcp` and requires a full rebuild.

**`pr_verify`**
Vivado command that checks a partial bitstream is compatible with its full
bitstream. Run automatically at the end of `dfx_impl.tcl` and `dfx_rm.tcl`.
Fails if the static region changed between builds.

**Decoupler**
The Xilinx DFX Decoupler IP (`xlnx_decoupler`) in the static region. When
asserted (`decouple=1` at register `0x5_0000`), it drives the RP-facing AXI
signals to a safe idle state so the bus doesn't hang during partial
reconfiguration. Always assert before loading a partial bitstream; deassert
after. `loomx decouple` / `loomx couple` do this from the host.

---

## Build Artifacts

```
work-u250/results/
├── static_synth.dcp        Static synthesis checkpoint (RP = black box)
├── static_routed.dcp       *** GOLDEN *** locked static P&R — keep this safe
├── full.bit                Full bitstream (static + initial RM) → SPI flash
├── shell.mcs               MCS for flash programming (generated from full.bit)
├── <RM_NAME>_synth.dcp     RM synthesis checkpoint
├── <RM_NAME>_partial.bit   Partial bitstream for JTAG DUT swap
├── <RM_NAME>_routed.dcp    RM routed checkpoint
└── *.rpt                   Utilisation / timing / DRC reports
```

`static_routed.dcp` is the most important file. Treat it like a compiled
artifact for the board — back it up alongside `full.bit` and `shell.mcs`. If
you delete it, the next `dfx-static` must redo the full place & route.

---

## Workflow

### One-time setup (per board)

```bash
# 1. Generate Xilinx IPs (downloads nothing, runs Vivado IP gen locally)
make ip

# 2. Build and lock the static shell — slow, run once
#    Produces: static_routed.dcp, full.bit, rm_partial.bit
make dfx-static TRANSFORMED_V=path/to/transformed.v

# 3. Burn full.bit to SPI flash — shell now loads on every power-on
make dfx-program-flash
```

### Per-DUT (fast path)

```bash
# Compile new DUT through the Loom transform pipeline first, then:

# 1. Build partial bitstream (OOC synth + partial P&R against locked static)
make dfx-rm TRANSFORMED_V=path/to/new_transformed.v RM_NAME=my_dut
#    → work-u250/results/my_dut_partial.bit

# 2. Load via JTAG — static shell is completely untouched
make dfx-program-rm PARTIAL_BIT=work-u250/results/my_dut_partial.bit

# 3. Run
loomx -work path/to/build -t xdma
```

`RM_NAME` is just a label for output file naming. Omitting it defaults to `rm`.

### When to rebuild the static shell

Only needed if you change the **static region** itself:

| Change | Need full rebuild? |
|--------|--------------------|
| New DUT (`transformed.v`) | No — use `dfx-rm` |
| `loom_emu_top` internals | No — use `dfx-rm` |
| `loom_shell.sv` (outside `u_emu_top`) | Yes — `dfx-static` |
| IP changes (XDMA, CDC, clk_gen) | Yes — `make ip` then `dfx-static` |
| Pblock changes (`u250_dfx.xdc`) | Yes — `dfx-static` |
| Timing / pin constraints | Yes — `dfx-static` |

---

## Make Targets

| Target | Description |
|--------|-------------|
| `make ip` | Generate all Xilinx IPs (one-time) |
| `make synth` | Non-DFX flat synthesis (for development/debug) |
| `make bitstream` | Non-DFX full bitstream (for development/debug) |
| `make dfx-static` | **DFX**: build & lock static shell (slow, run once) |
| `make dfx-rm` | **DFX**: build partial bitstream for a DUT (fast) |
| `make dfx-program-flash` | Write `full.bit` to SPI flash (shell persistence) |
| `make dfx-program-rm` | Load partial bitstream via JTAG (DUT swap) |
| `make program` | Non-DFX JTAG full-device program |
| `make driver` | Build XDMA kernel driver |
| `make driver-load` | Insert XDMA kernel module |
| `make rescan` | PCIe bus rescan after programming |
| `make clean` | Remove `work-u250/` and IP `.done` stamps |

---

## Directory Structure

```
fpga/
├── README.md               This file
├── Makefile                Build orchestration
├── boards/u250/
│   ├── settings.tcl        Part / board-part strings
│   ├── u250_pins.xdc       Pin assignments
│   ├── u250_timing.xdc     Timing constraints
│   ├── u250_implementation.xdc  Bitstream config (SPI x4, compression)
│   └── u250_dfx.xdc        DFX pblock definition (RP = 122/128 clock regions across all 4 SLRs (~95% of device))
├── ip/
│   ├── xlnx_xdma/          PCIe XDMA IP
│   ├── xlnx_clk_gen/       Clocking Wizard IP
│   ├── xlnx_cdc/           AXI Clock Converter IP
│   └── xlnx_decoupler/     DFX Decoupler IP
└── scripts/
    ├── synth.tcl            Non-DFX flat synthesis
    ├── synth_static.tcl     DFX: static synthesis (RP as black box)
    ├── synth_rm.tcl         DFX: RM OOC synthesis
    ├── impl.tcl             Non-DFX implementation
    ├── dfx_impl.tcl         DFX: full P&R → locks static_routed.dcp
    ├── dfx_rm.tcl           DFX: partial P&R using locked static
    ├── program.tcl          Non-DFX JTAG programming
    ├── dfx_program_flash.tcl  Write full.bit to SPI flash
    └── dfx_program_rm.tcl   JTAG partial bitstream load
```
