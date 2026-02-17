# SHUTDOWN.md — Loom Emulation Shutdown Mechanism

## Problem

A running Loom emulation (sim or FPGA) needs a clean way to stop. Three sources can trigger shutdown:

1. **Host-initiated** — test program is done, calls `loom_finish()`
2. **DUT-initiated** — the design hits `$finish` or `$fatal` in its RTL
3. **Timeout** — sim has run too long (handled externally by CTest / shell script)

All three must funnel through the same mechanism so that the host driver, the DPI server thread, and the simulator all shut down cleanly.

## Design

One register. One signal. One message type.

### EMU_FINISH Register

Added to `loom_emu_ctrl` at offset `0x4C`:

```
Bits     Field         Description
[0]      finish_req    Shutdown requested (write 1 to trigger)
[7:1]    reserved
[15:8]   exit_code     0 = success, nonzero = error/fatal
[31:16]  reserved
```

Anyone can write this register: the host via AXI, or the DUT logic (via a hardwired connection from transformed `$finish`/`$fatal` cells).

### Signal Flow

```
DUT $finish/$fatal cells ──┐
                           ├──► emu_ctrl.finish_req ──► BFM finish input ──► $finish
Host loom_finish() ────────┘         │
                                     └──► IRQ[2] (emu state change) ──► host notices
```

That's it. Everything converges on one register, one wire, one shutdown path.

---

## Part 1: Transforming `$finish` / `$fatal` in the DUT

### What Yosys Has After `proc`

Yosys represents `$finish` and `$fatal` as cells:

- **`$print`** — for `$display`, `$info`, `$warning`, `$error` (informational, no shutdown)
- **`$check`** — for `$fatal`, assertions, and similar (with a `FLAVOR` parameter)

`$finish` calls may appear as `$check` cells with specific flavors, or as process nodes. The exact representation depends on the frontend. With yosys-slang, `$finish` and `$fatal` may appear as:

- `$check` cells with `FLAVOR` = `"$fatal"` for `$fatal`
- Process-embedded `$finish` calls that get lowered during `proc`

### The Pass: `finish_transform`

A simple Yosys pass that runs **before** `emu_top`. It walks all modules and replaces `$finish`/`$fatal` cells with logic that writes to the EMU_FINISH register.

```
pass: finish_transform
runs: after proc, before emu_top

for each module:
  for each cell:
    if cell is $finish or $fatal:
      1. create a wire: finish_trigger_N (1 bit)
      2. connect the cell's trigger condition to finish_trigger_N
      3. remove the original cell
      4. add finish_trigger_N to a module-level collection

  create output port: loom_finish_req (1 bit)
  create output port: loom_finish_code (8 bits)

  loom_finish_req  = |{finish_trigger_0, finish_trigger_1, ...}
  loom_finish_code = (first $fatal trigger) ? fatal_code : 0
```

After this pass, the DUT has no `$finish`/`$fatal` cells — just output ports that signal shutdown requests. The `emu_top` pass wires these to `emu_ctrl.finish_req`.

### Implementation

```cpp
struct FinishTransformPass : public Pass {
    FinishTransformPass() : Pass("finish_transform",
        "Replace $finish/$fatal with finish request signals") {}

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing FINISH_TRANSFORM pass.\n");

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) break;
        extra_args(args, argidx, design);

        for (auto module : design->selected_modules())
            transform_module(module);
    }

    void transform_module(RTLIL::Module *module) {
        std::vector<RTLIL::Cell*> to_remove;
        std::vector<RTLIL::SigSpec> finish_triggers;
        std::vector<RTLIL::SigSpec> fatal_triggers;

        for (auto cell : module->cells()) {
            bool is_finish = false;
            bool is_fatal = false;
            int exit_code = 0;

            // Detect $finish / $fatal cells
            // The exact cell types depend on the frontend (yosys-slang).
            // Common patterns:
            if (cell->type == ID($check)) {
                std::string flavor = cell->getParam(ID::FLAVOR).decode_string();
                if (flavor == "$fatal" || flavor == "fatal") {
                    is_fatal = true;
                    // $fatal exit code is typically the first argument
                    // or encoded in a parameter
                }
            }

            // Also catch any remaining $finish cells if they exist
            // (frontend-dependent — may need adjustment for yosys-slang)

            if (is_finish || is_fatal) {
                // Extract the trigger condition
                // $check cells have an \EN port (the condition under which they fire)
                // and a \TRG port (trigger signal)
                RTLIL::SigSpec trigger;
                if (cell->hasPort(ID::EN)) {
                    trigger = cell->getPort(ID::EN);
                } else if (cell->hasPort(ID::TRG)) {
                    trigger = cell->getPort(ID::TRG);
                } else {
                    // Unconditional — use constant 1
                    trigger = RTLIL::SigSpec(RTLIL::State::S1);
                }

                if (is_fatal) {
                    fatal_triggers.push_back(trigger);
                } else {
                    finish_triggers.push_back(trigger);
                }

                to_remove.push_back(cell);
                log("  Replacing %s (%s) in %s\n",
                    log_id(cell), log_id(cell->type), log_id(module));
            }
        }

        if (to_remove.empty()) return;

        // Remove original cells
        for (auto cell : to_remove)
            module->remove(cell);

        // Create output ports
        RTLIL::Wire *finish_req = module->addWire(ID(\loom_finish_req), 1);
        finish_req->port_output = true;

        RTLIL::Wire *finish_code = module->addWire(ID(\loom_finish_code), 8);
        finish_code->port_output = true;

        // OR all triggers together → finish_req
        RTLIL::SigSpec all_triggers;
        for (auto &t : finish_triggers) all_triggers.append(t);
        for (auto &t : fatal_triggers) all_triggers.append(t);

        if (all_triggers.size() == 1) {
            module->connect(finish_req, all_triggers);
        } else {
            // Reduce-OR
            RTLIL::SigSpec reduced = module->ReduceOr(NEW_ID, all_triggers);
            module->connect(finish_req, reduced);
        }

        // Exit code: 0 for $finish, 1 for $fatal
        // If any fatal trigger fires, exit_code = 1, else 0
        if (!fatal_triggers.empty()) {
            RTLIL::SigSpec any_fatal;
            for (auto &t : fatal_triggers) any_fatal.append(t);
            RTLIL::SigSpec fatal_bit = module->ReduceOr(NEW_ID, any_fatal);
            // Zero-extend to 8 bits
            module->connect(finish_code,
                RTLIL::SigSpec({RTLIL::SigSpec(RTLIL::State::S0, 7), fatal_bit}));
        } else {
            module->connect(finish_code, RTLIL::SigSpec(RTLIL::State::S0, 8));
        }

        module->fixup_ports();
        log("  Transformed %zu $finish/$fatal cells, added loom_finish_req/code ports\n",
            to_remove.size());
    }
};
```

---

## Part 2: `emu_ctrl` Finish Logic

The `loom_emu_ctrl` module gains:

```systemverilog
// Inputs from DUT (wired by emu_top)
input  logic        dut_finish_req,
input  logic [7:0]  dut_finish_code,

// Output to BFM (sim only) / directly readable by host (FPGA)
output logic        finish
```

Logic:

```systemverilog
logic [15:0] finish_reg;  // mapped at offset 0x4C

// DUT-initiated finish
always_ff @(posedge clk) begin
    if (rst) begin
        finish_reg <= 16'h0;
    end else begin
        // DUT can set finish
        if (dut_finish_req && !finish_reg[0]) begin
            finish_reg[0]   <= 1'b1;
            finish_reg[15:8] <= dut_finish_code;
        end
        // Host can also set finish via AXI write (handled in AXI write logic)
    end
end

assign finish = finish_reg[0];
```

When the host writes to `EMU_FINISH` via AXI (offset `0x4C`), the same `finish_reg` is set. Both paths (DUT and host) converge on the same bit.

---

## Part 3: BFM Finish Handling

The BFM gains a `finish` input. When it goes high, the BFM:

1. Sends a `SHUTDOWN` message (type 3) to the host over the socket
2. Waits for any in-flight AXI transaction to complete
3. Closes the socket
4. Calls `$finish`

```systemverilog
// Addition to loom_axil_socket_bfm

input logic finish,

// ...

logic finish_pending;
logic shutdown_sent;

always_ff @(posedge clk) begin
    if (rst) begin
        finish_pending <= 0;
        shutdown_sent  <= 0;
    end else begin
        if (finish && !finish_pending) begin
            finish_pending <= 1;
        end

        if (finish_pending && !shutdown_sent && state == IDLE) begin
            loom_sock_send(8'd3, 32'd0, 32'd0);  // SHUTDOWN message
            shutdown_sent <= 1;
        end

        if (shutdown_sent && state == IDLE) begin
            loom_sock_close();
            $finish;
        end
    end
end
```

On FPGA, there is no BFM, so the `finish` signal is just ignored (or exposed as a GPIO/LED for debug). The host detects shutdown by reading `EMU_FINISH`.

---

## Part 4: Host Side

### Socket Transport: Handle SHUTDOWN Message

```c
// In socket_wait_irq and any recv path:
if (resp.type == 3) {
    // Sim is shutting down
    return false;  // signal to caller that transport is done
}
```

The DPI service loop in `loom_serve_dpi` checks for this and exits cleanly.

### `loom_finish()` API

```c
int loom_finish(loom_ctx_t *ctx, int exit_code) {
    uint32_t val = 0x01 | ((exit_code & 0xFF) << 8);
    loom_write32(ctx->tr, ctx->emu_ctrl_base + 0x4C, val);
    // In sim: this write goes through socket → BFM → AXI → emu_ctrl → finish → $finish
    // On FPGA: this just sets the register, host then closes normally
    return 0;
}
```

### Typical Test Program Flow

```c
int main() {
    // ... setup transport, open context, start DPI thread ...

    loom_reset(ctx);
    loom_start(ctx);

    // Option A: host decides when to stop
    sleep(5);
    loom_stop(ctx);
    // ... check results ...
    loom_finish(ctx, 0);

    // Option B: run until DUT finishes on its own
    while (1) {
        int status = loom_get_status(ctx);
        if (status == LOOM_STATUS_SHUTDOWN) {
            int code = loom_read32(ctx->tr, ctx->emu_ctrl_base + 0x4C);
            int exit_code = (code >> 8) & 0xFF;
            printf("DUT finished with exit code %d\n", exit_code);
            break;
        }
        usleep(10000);
    }

    // Option C: just call finish and let it propagate
    loom_finish(ctx, 0);

    loom_stop_dpi(ctx);
    pthread_join(dpi_tid, NULL);
    loom_close(ctx);
    return 0;
}
```

---

## Part 5: Wiring in `emu_top`

The `emu_top` pass connects everything:

```
DUT.loom_finish_req  ──► emu_ctrl.dut_finish_req
DUT.loom_finish_code ──► emu_ctrl.dut_finish_code
emu_ctrl.finish      ──► bfm.finish          (sim top only)
```

If the DUT has no `$finish`/`$fatal` cells (and therefore `finish_transform` added no ports), the `emu_top` pass ties `dut_finish_req` to `1'b0` and `dut_finish_code` to `8'h00`.

---

## Yosys Flow Integration

```tcl
read_slang design.sv --top my_soc --allow-dpi-bridge

proc
opt_clean

memory_collect
memory_dff

finish_transform      # ← NEW: $finish/$fatal → loom_finish_req/code ports
mem_shadow   -meta loom_meta/loom_mem_meta.json
scan_insert  -meta loom_meta/loom_scan_meta.json
loom_instrument   -meta loom_meta/loom_dpi_meta.json

emu_top -board xcu200 -pcie xdma -meta_dir loom_meta/

opt
clean
write_verilog -noattr loom_output.v
```

`finish_transform` runs early (after `proc`, before any Loom instrumentation) because it operates on the DUT's cells directly. By the time `scan_insert` and `emu_top` run, the `$finish`/`$fatal` cells are gone and the DUT has clean `loom_finish_req`/`loom_finish_code` output ports.

---

## Summary

| Component | What it does |
|-----------|-------------|
| `finish_transform` pass | Replaces `$finish`/`$fatal` cells with `loom_finish_req`/`loom_finish_code` output ports on the DUT |
| `emu_ctrl` | Has `EMU_FINISH` register at `0x4C`. Set by DUT ports or by host AXI write. Drives `finish` output signal. |
| BFM `finish` input | When high: send SHUTDOWN message, drain AXI, `$finish` |
| Host `loom_finish()` | Writes `EMU_FINISH` register. In sim, this triggers the BFM→`$finish` chain. On FPGA, host just closes. |
| Socket SHUTDOWN message | Type 3, sent BFM→host. Host transport returns cleanly. |

Five pieces, each trivial on its own, composing into a complete shutdown path for all three scenarios (host-initiated, DUT-initiated, timeout).
