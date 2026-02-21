<!-- SPDX-License-Identifier: Apache-2.0 -->
# Transformation Pass Pipeline

Loom transforms simulation-grade SystemVerilog (including DPI-C calls, `$display`,
`$finish`, timing blocks, and asynchronous resets) into FPGA-synthesizable RTL
with a host communication wrapper. The transformation is a sequence of Yosys
passes plus a frontend extension.

## Pipeline Overview

```
                              yosys-slang frontend (--loom)
                              ───────────────────────────────
Source SV ─── read_slang ───► Elaborate, detect DPI, extract FSMs
                              ▼
                          hierarchy / proc / opt / flatten
                              ▼
                          ┌───────────────┐
                          │ reset_extract │  Strip async resets, record initial values
                          └───────┬───────┘
                                  ▼
                          ┌───────────────────┐
                          │ loom_instrument   │  DPI bridge, FF enable, $finish transform
                          └───────┬───────────┘
                                  ▼
                          ┌───────────────┐
                          │ scan_insert   │  Scan chain, protobuf state map
                          └───────┬───────┘
                                  ▼
                          ┌───────────────┐
                          │ emu_top       │  Emulation wrapper with AXI-Lite interface
                          └───────┬───────┘
                                  ▼
                          write_verilog ──► transformed.v
```

Optional: `mem_shadow` runs before `flatten` to add shadow ports to memories.

### `loomc` orchestration

The `loomc` tool generates and executes this Yosys script:

```tcl
read_slang --loom [sources] --top <module>
hierarchy -check -top <module>
proc
opt
flatten
opt
reset_extract -rst rst_ni
opt
loom_instrument -header_out loom_dpi_dispatch.c
scan_insert -map scan_map.pb
emu_top -top <module> -rst rst_ni
opt_clean
write_verilog -noattr transformed.v
```

---

## Frontend: FSM Extraction

**Location:** `third_party/yosys-slang/src/fsm_extract.cc`

The yosys-slang frontend, when invoked with `--loom`, automatically extracts
finite state machines from simulation-style procedural blocks. This runs
during elaboration, before any Loom passes.

### What it handles

Multi-cycle `always @(posedge clk)` blocks containing inner timing controls:

```systemverilog
always @(posedge clk) begin
    request <= 1;
    @(posedge clk);                    // explicit state boundary
    while (!ack) @(posedge clk);       // while-wait loop
    repeat (N) @(posedge clk);         // counted delay
    request <= 0;
end
```

These patterns are legal in simulation but not synthesizable. The FSM
extractor converts them into explicit state machines with state registers
and transition logic.

### Transformation patterns

| Source pattern | Generated RTL |
|---------------|---------------|
| `@(posedge clk)` | State boundary — new state created |
| `while (!cond) @(posedge clk)` | Wait state with self-loop until `cond` |
| `repeat (N) { body; @(posedge clk); }` | Counter register, decrement loop |
| `if (cond) { ...; @(posedge clk); }` | Conditional branch with timing |

### How it works

1. **Timing detection** — `TimingDetector` AST visitor identifies inner
   `@(posedge clk)` timing controls within `always @(posedge clk)` blocks
2. **FSM graph construction** — `FsmBuilder` walks the procedural body,
   creating states at each timing boundary. Actions (assignments) are
   accumulated on state transitions.
3. **RTL generation** — `emit_multicycle_fsm` creates:
   - State register (`loom_fsm_state`, `ceil_log2(N)` bits wide)
   - Counter registers for `repeat` loops
   - DFFs for all driven signals
   - Switch-based transition logic

### Repeat-zero bypass

`repeat(0)` must not enter the loop body at all. The extractor generates
bypass logic: if the count expression is zero, the transition skips the
repeat state entirely.

### Equivalence testing

`tests/fsm_extract/` contains auto-discovery equivalence tests. The
`gen_equiv.tcl` script:

1. Transforms `*_fsm.sv` files through yosys-slang
2. Generates a testbench pairing the transformed (DUT) and original
   (reference) modules
3. Runs 1000 random-stimulus cycles comparing outputs

```bash
cd tests/fsm_extract && make sim
```

---

## Pass 1: `reset_extract`

**Location:** `passes/reset_extract/reset_extract.cc`

Strips asynchronous resets from the design and records their initial values
as wire attributes. This converts the design from "reset-initialized" to
"scan-initialized" — the host scans in the initial state image before the
first emulation run instead of using a hardware reset.

### Usage

```tcl
reset_extract -rst <signal_name>
```

### What it does

1. **Strip async resets** — converts async-reset FFs to plain FFs:
   - `$adff` → `$dff` (async reset → no reset)
   - `$adffe` → `$dffe` (async reset + enable → enable only)
   - `$dffsr` → `$dff` (set/reset → no reset)
   - `$dffsre` → `$dffe` (set/reset + enable → enable only)

2. **Record initial values** — stores the original reset value as a
   `loom_reset_value` wire attribute on each FF's Q output. These values
   are later used by `scan_insert` to build the initial scan image.

3. **Keep sync resets** — `$sdff`, `$sdffe`, `$sdffce` are left intact.
   They become regular combinational logic after `opt` (dead code since
   the reset port is tied inactive).

4. **Remove the reset port** — drives the reset input to constant `1`
   (inactive for active-low `rst_ni`), which `opt` then eliminates.

### DPI in reset blocks

When an `$aldff`/`$aldffe` has its async-data port driven by a DPI call:

```systemverilog
always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) reg_q <= get_init_val(42);
    else         reg_q <= reg_q + 1;
```

The pass:
- Traces the AD port to the `$__loom_dpi_call` cell
- Marks it with `loom_dpi_reset = true`
- Stores the function name as `loom_reset_dpi_func` on the Q wire
- Strips the cell to `$dff`/`$dffe`

At runtime, the host executes the DPI function and patches the return value
into the scan image at the recorded bit offset before scanning in.

### Module attributes set

| Attribute | Value | Consumer |
|-----------|-------|----------|
| `loom_resets_extracted` | `"1"` | `emu_top` (verifies reset was processed) |

---

## Pass 2: `loom_instrument`

**Location:** `passes/loom_instrument/loom_instrument.cc`

Converts DPI call cells into a hardware bridge, transforms `$finish` cells
into an output signal, and adds flip-flop enable logic so the DUT can be
frozen while the clock continues running.

### Usage

```tcl
loom_instrument [-gen_wrapper] [-header_out file.c]
```

### DPI bridge

The yosys-slang frontend creates `$__loom_dpi_call` cells for each DPI
import call site. This pass converts them into a hardware interface:

1. **Assigns function IDs** — each call site gets a unique `func_id`
   (0..N-1). Call sites in runtime code get hardware bridge entries;
   call sites in `initial` blocks get dispatch-only entries.

2. **Builds multiplexed interface** — when multiple DPI calls exist:
   - `loom_dpi_valid` = OR of all valid conditions
   - `loom_dpi_func_id` = priority-encoded mux selecting active function's ID
   - `loom_dpi_args` = mux selecting active function's packed arguments

3. **Generates dispatch code** — `loom_dpi_dispatch.c` with per-call-site
   wrappers and the `loom_dpi_funcs[]` table (see
   [dpi-bridge-internals.md](dpi-bridge-internals.md) for details).

### `$finish` transformation

Collects all `$__loom_finish` cells, ORs their enable conditions, and
creates `loom_finish_o` output port. The original cells are removed.

### FF enable (loom_en)

Adds `loom_en` input port and modifies every flip-flop's enable:

```
EN_effective = (loom_en & original_EN) | loom_scan_enable
```

- `loom_en = 0` → DUT frozen (waiting for DPI or not running)
- `loom_scan_enable` overrides to ensure scan always works
- Memory output FFs are skipped (already handled by memory interface)

### Ports created

| Port | Dir | Width | Description |
|------|-----|-------|-------------|
| `loom_en` | in | 1 | FF enable (freezes DUT when low) |
| `loom_dpi_valid` | out | 1 | DPI call pending |
| `loom_dpi_func_id` | out | 8 | Function ID |
| `loom_dpi_args` | out | max input width | Packed input arguments |
| `loom_dpi_result` | in | 64 + max output width | Return + output array data |
| `loom_finish_o` | out | 1 | Finish request (OR of all `$finish`) |

### Module attributes set

| Attribute | Value | Consumer |
|-----------|-------|----------|
| `loom_n_dpi_funcs` | function count | `emu_top` (regfile sizing) |

---

## Pass 3: `scan_insert`

**Location:** `passes/scan_insert/scan_insert.cc`

Inserts scan chain multiplexers on all flip-flops, creating a serial
shift path for state capture and restore.

### Usage

```tcl
scan_insert [-chain_length N] [-map file.pb] [-check_equiv]
```

### What it does

1. **Insert scan muxes** — each FF gets a mux on its D input:
   - `loom_scan_enable = 0` → normal logic drives D
   - `loom_scan_enable = 1` → scan data from previous FF drives D

2. **Build serial chain** — FFs are chained: `loom_scan_in` → FF₀.D → FF₀.Q
   → FF₁.D → ... → FFₙ.Q → `loom_scan_out`. The chain is bit-serial
   (each bit of an N-bit FF connects in sequence).

3. **Generate scan map** — a protobuf file (`scan_map.pb`) mapping each
   variable's name, width, and bit offset in the chain. Also includes:
   - Reset values from `loom_reset_value` attributes
   - Initial DPI call entries (from `loom_dpi_initial` / `loom_dpi_reset`)
   - Enum member names (for debug display)
   - Packed initial scan image (all reset values concatenated)

### Ports created

| Port | Dir | Width | Description |
|------|-----|-------|-------------|
| `loom_scan_enable` | in | 1 | Scan mode enable |
| `loom_scan_in` | in | 1 | Serial data in |
| `loom_scan_out` | out | 1 | Serial data out |

### Module attributes set

| Attribute | Value | Consumer |
|-----------|-------|----------|
| `loom_scan_chain_length` | total bits | `emu_top` (scan controller sizing) |

### Equivalence checking

With `-check_equiv`, the pass creates a gold (pre-scan) and gate (post-scan
with scan tied to 0) copy and runs Yosys `equiv_*` commands to verify
functional equivalence.

---

## Pass 4: `mem_shadow` (optional)

**Location:** `passes/mem_shadow/mem_shadow.cc`

Adds shadow read/write ports to `$mem_v2` cells for random-access host
access to memory contents. Much faster than serial scan for large memories.

### Usage

```tcl
mem_shadow [-map file.json] [-ctrl module_name]
```

### When to use

Run **before** `flatten` and **before** `memory_bram` (must operate on
`$mem_v2` cells, not BRAM primitives):

```tcl
memory_collect
memory_dff
mem_shadow -map mem_map.json
flatten
```

### What it does

- Adds shadow read/write port to each memory
- Generates `loom_mem_ctrl` module with address decode logic
- Creates a memory address map JSON for the host driver
- Address space is word-addressed (4 bytes per word for AXI alignment)

---

## Pass 5: `emu_top`

**Location:** `passes/emu_top/emu_top.cc`

Generates the complete emulation wrapper (`loom_emu_top`) with all
infrastructure needed for host communication.

### Usage

```tcl
emu_top -top <module> [-clk name] [-rst name] [-addr_width N] [-n_irq N]
```

### Generated architecture

```
loom_emu_top
├── u_interconnect (loom_axil_demux)  AXI-Lite 1:3 demux
├── u_emu_ctrl     (loom_emu_ctrl)    Emulation FSM + DPI bridge
├── u_dpi_regfile  (loom_dpi_regfile) Per-function DPI registers
├── u_scan_ctrl    (loom_scan_ctrl)   Scan chain controller
└── u_dut          (original DUT)     Transformed design
```

### AXI-Lite address map

| Range | Target |
|-------|--------|
| `0x0_0000 – 0x0_FFFF` | loom_emu_ctrl |
| `0x1_0000 – 0x1_FFFF` | loom_dpi_regfile |
| `0x2_0000 – 0x2_FFFF` | loom_scan_ctrl |

See [emu-top.md](emu-top.md) for the full register map.

### Auto-detection

The pass reads module attributes set by earlier passes:

| Attribute | Source pass | Used for |
|-----------|-----------|----------|
| `loom_n_dpi_funcs` | `loom_instrument` | DPI regfile sizing |
| `loom_scan_chain_length` | `scan_insert` | Scan controller sizing |
| `loom_resets_extracted` | `reset_extract` | Verification |
| `loom_tbx_clk` | yosys-slang | Clock port detection |

### DUT connection

- **Clock**: connected to ungated `clk_i` (free-running)
- **Reset**: must have been removed by `reset_extract` (error otherwise)
- **`loom_en`**: from `emu_ctrl` (combinational fast path)
- **DPI signals**: connected to `emu_ctrl`'s DPI forwarding
- **Scan signals**: connected to `scan_ctrl`
- **Other inputs**: tied to constant 0
- **Other outputs**: left unconnected

### IRQ generation

The wrapper generates an interrupt output:

```
irq_o[0] = |dpi_stall    (OR-reduce of all per-function stall signals)
```

`dpi_stall[i]` is high when function `i` has a pending call waiting for
the host. This interrupt drives the socket BFM (which sends it as a type-2
message to the host) or the FPGA MSI path.

---

## Data Flow Example

Consider a DUT with one DPI call and one `$finish`:

### After `reset_extract`

```diff
- $adff  CLK=clk_i  ARST=!rst_ni  ARST_VALUE=0  D=...  Q=counter_q
+ $dff   CLK=clk_i  D=...  Q=counter_q
  // counter_q wire now has loom_reset_value = 0
  // rst_ni port driven to 1'b1
```

### After `loom_instrument`

```diff
+ port loom_en (input, 1-bit)
+ port loom_dpi_valid (output, 1-bit)
+ port loom_dpi_func_id (output, 8-bit)
+ port loom_dpi_args (output, 64-bit)
+ port loom_dpi_result (input, 96-bit)
+ port loom_finish_o (output, 1-bit)
  // $__loom_dpi_call cells removed, bridge mux logic added
  // $__loom_finish cells removed, finish_o = OR of enables
  // FF enables: EN = (loom_en & old_EN) | loom_scan_enable
```

### After `scan_insert`

```diff
+ port loom_scan_enable (input, 1-bit)
+ port loom_scan_in (input, 1-bit)
+ port loom_scan_out (output, 1-bit)
  // Scan muxes on all FF D inputs
  // scan_map.pb written with counter_q at offset 0, width 32, reset_value=0
```

### After `emu_top`

```diff
+ module loom_emu_top
+   u_interconnect: loom_axil_demux (3 masters)
+   u_emu_ctrl: loom_emu_ctrl (state machine, DPI bridge, loom_en)
+   u_dpi_regfile: loom_dpi_regfile (1 function, 8 args max)
+   u_scan_ctrl: loom_scan_ctrl (32-bit chain)
+   u_dut: original DUT (all loom_* ports connected)
+   irq_o[0] = |dpi_stall
+   finish_o = emu_ctrl.finish_o
```

---

## Attribute and Signal Reference

### Wire attributes (set on FF Q outputs)

| Attribute | Set by | Description |
|-----------|--------|-------------|
| `loom_reset_value` | `reset_extract` | Original async reset value |
| `loom_reset_dpi_func` | `reset_extract` | DPI function for reset-computed values |

### Module attributes

| Attribute | Set by | Description |
|-----------|--------|-------------|
| `loom_resets_extracted` | `reset_extract` | Confirms reset processing done |
| `loom_n_dpi_funcs` | `loom_instrument` | Total DPI function count |
| `loom_scan_chain_length` | `scan_insert` | Total scan chain bits |
| `loom_tbx_clk` | yosys-slang | Auto-detected clock port name |

### Signal naming convention

All Loom-generated signals use the `loom_` prefix:

| Pass | Signals |
|------|---------|
| `loom_instrument` | `loom_en`, `loom_dpi_valid`, `loom_dpi_func_id`, `loom_dpi_args`, `loom_dpi_result`, `loom_finish_o` |
| `scan_insert` | `loom_scan_enable`, `loom_scan_in`, `loom_scan_out` |
| `mem_shadow` | `loom_shadow_*_addr`, `loom_shadow_*_rdata`, `loom_shadow_*_wdata`, `loom_shadow_*_wen`, `loom_shadow_clk` |
