<!-- SPDX-License-Identifier: Apache-2.0 -->
# Synthesizable Assertions

Loom synthesizes SystemVerilog assertions into hardware that prints a
failure message on the host and halts emulation. No special macros are
needed — standard `assert`, `assert property`, and else clauses all work.

## Supported Constructs

| Construct | Example | Support |
|-----------|---------|---------|
| Immediate assert | `assert (cond);` | Full |
| Immediate assert with else | `assert (cond) else $error("msg");` | Full |
| Immediate assume | `assume (cond);` | Removed (no emulation meaning) |
| Immediate cover | `cover (cond);` | Removed |
| Concurrent assert (single-cycle) | `assert property (@(posedge clk) disable iff (rst) expr);` | Full |
| Concurrent assume | `assume property (...);` | Removed |
| Concurrent cover | `cover property (...);` | Removed |
| Sequence delay (`##N`) | `assert property (@(posedge clk) a ##1 b);` | Full (exact delays) |
| Overlapping implication (`\|->`) | `assert property (@(posedge clk) a \|-> b ##1 c);` | Full |
| Non-overlapping implication (`\|=>`) | `assert property (@(posedge clk) a \|=> b);` | Full |
| `$rose` / `$fell` / `$stable` | `if ($rose(en)) ...` | Full (clocked context) |
| Range delays (`##[M:N]`) | `assert property (a ##[1:3] b);` | Not yet supported |
| Repetition (`[*N]`) | `assert property (a [*3]);` | Not yet supported |
| Unbounded / liveness | `s_eventually`, `[*]` | Not synthesizable |

Immediate assertions can appear in `always_comb`, `always_ff`, or
`always` blocks. Concurrent assertions can appear at module scope or
inside procedural blocks.

The lowRISC `ASSERT` macro pattern works directly:

```systemverilog
label: assert property (@(posedge clk_i) disable iff (!rst_ni) prop)
    else begin
        $error("assertion failed");
    end
```

## Pipeline

```
yosys-slang                 Yosys built-in             loom_instrument
──────────                  ──────────────             ───────────────
assert(cond)           →  $check cell    ─┐
assert property(...)   →  $check cell     │  async2sync    $assert → DPI display
                                          ├─ ──────────► ──────────► + finish
assert(...) else $error→  $check + $print │  chformal      $print  → DPI display
                                          │  -lower
assume/cover           →  $check cell    ─┘               removed (no-op)
```

### Step-by-step

1. **Frontend (yosys-slang):** Assertions become `$check` cells with
   ports `A` (condition), `EN` (enable), `TRG` (clock trigger), and
   params `FLAVOR` ("assert"/"assume"/"cover"). Else clauses on
   immediate assertions create additional `$print` cells conditioned
   on `!A`. This is not behind `--loom`; it is a general yosys-slang
   feature.

2. **`async2sync`:** Converts edge-triggered `$check`/`$print` cells
   (from `always_ff` contexts) to level-triggered by adding sampling
   FFs. Runs after `reset_extract`, so no `$adff` cells remain to
   interfere.

3. **`chformal -lower`:** Splits each `$check` cell into:
   - A `$assert` cell (condition + enable)
   - A `$print` cell (if FORMAT/ARGS are non-empty)

4. **`loom_instrument`:**
   - **`process_assert_cells()`** handles `$assert` cells:
     - Computes `fail = EN & !A`
     - Creates a `$__loom_dpi_call` with message
       `"Assertion failed: <name> [<source>]"`
     - Collects fail signals for finish
   - **Removes** `$assume`, `$cover`, `$live`, `$fair` cells (no
     emulation meaning; Verilator would otherwise treat them as
     assertions)
   - **`process_print_cells()`** handles `$print` cells from else
     clauses (same path as `$display`)
   - **`process_finish_cells()`** ORs assertion fail signals with any
     `$__loom_finish` signals into `loom_finish_o`

### `loomc` script

The relevant section of the generated Yosys script:

```tcl
flatten
reset_extract -rst rst_ni
async2sync
chformal -lower
loom_instrument -header_out loom_dpi_dispatch.c
```

## Assertion Failure at Runtime

When an assertion fails during emulation:

1. The assertion's DPI function fires (`__loom_assert_N`), printing:
   ```
   Assertion failed: c1_no_error [dut.sv:42.5-42.38]
   ```
2. `loom_finish_o` goes high
3. `emu_ctrl` latches the finish request and transitions to `StFrozen`
4. The BFM sends SHUTDOWN to the host
5. `loomx` exits

This reuses the existing `$display` → DPI bridge and `$finish` → shutdown
infrastructure. See [dpi-bridge-internals.md](dpi-bridge-internals.md) and
[shutdown-spec.md](shutdown-spec.md).

## Concurrent SVA: What is Supported

The yosys-slang frontend compiles SVA properties into checker RTL
(shift registers and combinational logic) that produces a 1-bit check
signal for the existing `$check` cell mechanism. Supported patterns:

### Single-cycle properties

```
ClockingAssertionExpr (@posedge/negedge clk)
  └─ optional DisableIffAssertionExpr (disable iff condition)
      └─ SimpleAssertionExpr (boolean expression, no repetition)
```

### Finite sequences (`##N` delays)

```systemverilog
assert property (@(posedge clk) a ##1 b ##2 c);
```

Each `##N` delay creates N flip-flops to delay the preceding
condition. The check signal is the AND of all delayed conditions,
verified at the completion tick. A valid pipeline (shift register
of constant `1` with zero-init) gates the enable for the first N
cycles while the delay chain warms up.

### Implications (`|->`, `|=>`)

```systemverilog
assert property (@(posedge clk) req |-> ##1 ack);     // overlapping
assert property (@(posedge clk) valid |=> data_ok);   // non-overlapping
```

The antecedent is delayed to align with the consequent's completion
tick. The check is `!ante_delayed | cons_match` (vacuously true when
the antecedent is false).

### Edge functions (`$rose`, `$fell`, `$stable`)

Available in clocked procedural blocks:

```systemverilog
always_ff @(posedge clk) begin
    if ($rose(valid)) assert (data != 0);
end
```

Each creates a single DFF to sample the previous value:
- `$rose(x)` = `x & !x_prev`
- `$fell(x)` = `!x & x_prev`
- `$stable(x)` = `x == x_prev`

### Clock and reset extraction

For `assert property (@(posedge clk_i) disable iff (!rst_ni) expr)`:

| AST node | Extracted value | Maps to |
|----------|----------------|---------|
| `ClockingAssertionExpr.clocking` | `clk_i`, posedge | `$check` TRG port, TRG_POLARITY |
| `DisableIffAssertionExpr.condition` | `!rst_ni` | `$check` EN = `!disable_cond` |
| Lowered property expression | checker RTL output | `$check` A port |

### Not yet supported

Unsupported constructs emit `SVAUnsupported` at compile time:
- Range delays: `##[M:N]`
- Repetition: `[*N]`, `[*M:N]`, `[->N]`
- Nested multi-cycle sub-sequences in concatenation
- Unbounded/liveness operators: `s_eventually`, `always`, `[*]`

## Else Clause Handling

Immediate assertions with else clauses are supported:

```systemverilog
always_ff @(posedge clk_i) begin
    assert (state_q != StError)
        else $error("illegal state %0d!", state_q);
end
```

The frontend visits the else clause in a conditional context where the
enable is `!A` (assertion failed). Any `$display`, `$error`, `$warning`,
or `$info` calls become `$print` cells conditioned on failure. These
flow through the standard `$print` → DPI display path.

**Known limitation:** The else clause's DPI call and the assertion's
default failure DPI call fire simultaneously. The DPI bridge services
them sequentially, but `loom_finish_o` also fires in the same cycle.
Depending on emu_ctrl timing, the else clause message may not be
printed before shutdown completes. The default message
(`"Assertion failed: <name> [<src>]"`) is always printed.

## Signal Summary

| Signal / Attribute | Source | Description |
|---|---|---|
| `$check` cell | yosys-slang | Formal check (FLAVOR, A, EN, TRG) |
| `$assert` cell | `chformal -lower` | Lowered assertion (A, EN) |
| `$print` cell | `chformal -lower` or else clause | Failure message |
| `__loom_assert_N` | `loom_instrument` | DPI display for assertion N |
| `loom_finish_o` | `loom_instrument` | OR of all `$finish` + assertion failures |

## Test

`tests/e2e_assert/` contains an end-to-end test with:

- Concurrent SVA assertions (`assert property @(posedge clk) disable iff ...`)
- Sequence assertions with `##N` delays and `|->` / `|=>` implications
- `$rose` edge detection in clocked blocks
- Immediate assertions in `always_comb` and `always_ff`
- An else clause with `$error`
- An `assume property` (verified to be silently removed)
- `$display` progress markers (verifies existing infrastructure coexists)

A deliberate bug in the state machine triggers assertion failure at
cycle ~21, printing the failure message and halting emulation.
