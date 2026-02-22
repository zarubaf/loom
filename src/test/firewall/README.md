<!-- SPDX-License-Identifier: Apache-2.0 -->
# AXI-Lite Firewall Standalone Testbench

Standalone testbench for `loom_axil_firewall`. Exercises the firewall's full
state-space without the loom_shell/loomx stack by instantiating the firewall
directly, driven by the socket BFM and a C test driver.

## Architecture

```
fw_test_driver.c  ──(unix socket)──►  loom_axil_socket_bfm
                                            │
                                      loom_axil_demux (N=3)
                                       /      │       \
                                     M0       M1       M2
                                      │        │        │
                                 fw.s_axi  fw.s_mgmt  slave.ctrl
                                      │                  │
                                 fw.m_axi ──────►  slave.data
```

### Address Map (20-bit)

| Master | Range             | Target                                    |
|--------|-------------------|-------------------------------------------|
| 0      | `0x00000–0x0FFFF` | Firewall `s_axi` (data path → downstream) |
| 1      | `0x10000–0x1FFFF` | Firewall `s_mgmt` (management registers)  |
| 2      | `0x20000–0x2FFFF` | `fw_test_slave` control registers          |

## Files

| File                    | Description                                        |
|-------------------------|----------------------------------------------------|
| `tb_axil_firewall.sv`   | Top-level Verilator testbench (clock, reset, wiring, SV assertions) |
| `fw_test_slave.sv`      | Controllable downstream AXI-Lite slave (stall, drain, unsolicited modes) |
| `fw_test_driver.c`      | C test driver — connects via socket, runs 12 tests  |
| `Makefile`              | Standalone build and test orchestration              |

## Test Slave Modes

The `fw_test_slave` data port behavior is controlled via the MODE register
(`SLAVE_CTRL + 0x00`):

| Mode | Name       | Accepts requests? | Generates responses?        |
|------|------------|--------------------|-----------------------------|
| 0    | NORMAL     | Yes (when idle)    | After DELAY cycles          |
| 1    | STALL      | Yes (always)       | Never (queues internally)   |
| 2    | DRAIN      | No                 | Drains queued, 1/cycle      |
| 3    | UNSOL_RD   | No                 | 1 unsolicited RVALID pulse  |
| 4    | UNSOL_WR   | No                 | 1 unsolicited BVALID pulse  |

## Test Suite

| #  | Test                        | What it exercises                                      |
|----|-----------------------------|--------------------------------------------------------|
| 1  | `test_register_defaults`    | Read all 9 mgmt registers, verify reset values         |
| 2  | `test_register_readback`    | Write/readback writable registers with non-default values |
| 3  | `test_normal_read_write`    | Read/write through firewall with responsive slave      |
| 4  | `test_read_timeout`         | Stall slave, read → synthetic response, check TIMEOUT_COUNT |
| 5  | `test_write_timeout`        | Stall slave, write → synthetic BRESP, check TIMEOUT_COUNT |
| 6  | `test_unsolicited_after_timeout` | Stall → timeout → drain → check UNSOLICITED_COUNT |
| 7  | `test_force_unsolicited`    | Slave UNSOL_RD/UNSOL_WR modes, verify UNSOLICITED_COUNT |
| 8  | `test_lockdown`             | Set/clear CTRL.lockdown, verify STATUS.locked          |
| 9  | `test_decouple`             | Set/clear CTRL.decouple, verify STATUS.decouple_status |
| 10 | `test_clear_counts`         | Generate counts, CTRL.clear_counts, verify zeroed      |
| 11 | `test_custom_timeout_response` | Change RESP/RDATA_ON_TIMEOUT, verify synthetic data |
| 12 | `test_irq`                  | Enable timeout IRQ, trigger timeout, check IRQ arrives via socket |

## SV Assertions

Two immediate assertions run every clock cycle (Verilator `--assert`):

- **A1**: Lockdown blocks upstream — `arready` and `awready` must be deasserted
  when `ctrl_lockdown_q` is set.
- **A2**: Outstanding limit — `wr_outstanding_q` and `rd_outstanding_q` must
  never exceed `max_outstanding_q[3:0]`.

## Running

```bash
# Standalone (from this directory)
make test

# Via CTest (from repo root)
ctest --test-dir build -R axil_firewall --output-on-failure
```

The `test` target starts the Verilator sim in the background, waits for the
BFM socket to appear, runs the C driver, and waits for the sim to exit.
The C driver prints per-test PASS/FAIL and exits non-zero on any failure.

## Debugging

FST waveform dumps are written to `build/obj_dir/dump.fst`. Enable BFM
trace logging with `+loom_bfm_trace` on the sim command line:

```bash
build/obj_dir/Vtb_axil_firewall +socket=build/fw_test.sock +loom_bfm_trace
```

The watchdog defaults to 50ms (`+timeout=50000000` ns). Override with
`+timeout=<ns>` for longer debug sessions.
