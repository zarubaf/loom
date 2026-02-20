<!-- SPDX-License-Identifier: Apache-2.0 -->
# Shutdown Mechanism

A running Loom emulation (sim or FPGA) needs a clean way to stop. Three
sources can trigger shutdown:

1. **DUT-initiated** — the design hits `$finish` or `$fatal`
2. **Host-initiated** — the host writes the `EMU_FINISH` register
3. **Timeout** — handled externally (CTest, shell script, `loomx -timeout`)

All three funnel through one register and one signal.

## EMU_FINISH Register

Located in `loom_emu_ctrl` at offset `0x38`:

```
Bits     Field         Description
[0]      finish_req    Shutdown requested (write 1 to trigger)
[7:1]    reserved
[15:8]   exit_code     0 = success, nonzero = error/fatal
[31:16]  reserved
```

This register is set either by the DUT (via hardwired `dut_finish_req_i`
input) or by the host (via AXI-Lite write). Both paths converge on the
same register.

## Signal Flow

```
DUT $finish cells (loom_finish_o) ──┐
                                     ├──► emu_ctrl.finish_reg ──► BFM finish_i ──► $finish
Host writes EMU_FINISH ──────────────┘         │
                                               └──► finish_o
```

## DUT Side: `$finish` → `loom_finish_o`

The yosys-slang frontend (in `--loom` mode) converts `$finish` and
`$fatal` statements into `$__loom_finish` cells. The `loom_instrument`
pass then transforms these cells into a hardware output signal:

1. Collects all `$__loom_finish` cells in the module
2. Extracts each cell's `EN` port (the condition under which it fires)
3. ORs all enable signals into a single `loom_finish_o` output port
4. Removes the original cells

This runs as part of `loom_instrument`, not as a separate pass. The
`emu_top` pass then wires `loom_finish_o` to `emu_ctrl.dut_finish_req_i`.

If the DUT has no `$finish`/`$fatal` cells, `emu_top` ties
`dut_finish_req_i` to `1'b0`.

## Controller: `emu_ctrl`

When `dut_finish_req_i` goes high (or the host writes to `EMU_FINISH`),
the controller latches the finish register. The `finish_o` output goes
high and stays high.

```systemverilog
// DUT-initiated finish
if (dut_finish_req_i && !finish_reg_q[0]) begin
    finish_reg_d[0]    = 1'b1;
    finish_reg_d[15:8] = dut_finish_code_i;
end

// Host-initiated finish (AXI write to EMU_FINISH)
if (wr_finish_en && !finish_reg_q[0]) begin
    finish_reg_d = wr_finish_data;
end

assign finish_o = finish_reg_q[0];
```

## BFM: Simulation Shutdown

The `loom_axil_socket_bfm` has a `finish_i` input. When it goes high:

1. Waits for any in-flight AXI transaction to complete
2. Sends a `SHUTDOWN` message (type 3) to the host over the socket
3. Asserts `shutdown_o`
4. Calls `$finish` to end the Verilator simulation

## Host Side

### Socket Transport

The socket transport recognizes the `SHUTDOWN` message (type 3) and
returns cleanly. The DPI service loop detects this and exits.

### `Context::finish()` API

The host can also initiate shutdown:

```cpp
Result<void> Context::finish(int exit_code) {
    uint32_t val = 0x01 | ((exit_code & 0xFF) << 8);
    return write32(addr::EmuCtrl + reg::Finish, val);
}
```

In simulation, this write propagates: host → socket → BFM → AXI →
`emu_ctrl` → `finish_o` → BFM `finish_i` → `$finish`.

On FPGA, this sets the register directly. The host then disconnects.

### FPGA Mode

On FPGA there is no BFM. The `finish_o` signal is unused. The host
detects shutdown by reading `EMU_FINISH`. After writing the finish
register, the host closes the XDMA transport normally.

## Summary

| Component | Role |
|-----------|------|
| `loom_instrument` | Transforms `$__loom_finish` cells → `loom_finish_o` output port |
| `emu_top` | Wires `loom_finish_o` → `emu_ctrl.dut_finish_req_i` |
| `emu_ctrl` | `EMU_FINISH` register at `0x38`. Set by DUT or host. Drives `finish_o`. |
| BFM `finish_i` | When high: drain AXI, send SHUTDOWN, `$finish` |
| `Context::finish()` | Writes `EMU_FINISH` register |
| Socket SHUTDOWN | Message type 3, BFM → host. Transport returns cleanly. |
