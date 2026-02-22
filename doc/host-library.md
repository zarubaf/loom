<!-- SPDX-License-Identifier: Apache-2.0 -->
# Host Library

The Loom host library provides a C++ API for communicating with an emulated
design, servicing DPI function calls, and an interactive shell for
debugging.

## Components

```
src/host/
├── loom.h                    # Main API header (Context, Transport)
├── loom.cpp                  # Core library implementation
├── loom_transport_socket.cpp # Unix socket transport
├── loom_transport_xdma.cpp   # PCIe/XDMA transport (FPGA)
├── loom_dpi_service.h/cpp    # Generic DPI service loop
├── loom_shell.h/cpp          # Interactive shell (replxx-based)
├── loom_sim_main.cpp         # Main entry point
├── loom_vpi.cpp              # VPI implementation ($finish/$stop)
└── loom_log.h                # Header-only logging
```

## Interactive Shell

`loomx` starts an interactive shell by default. The shell provides tab
completion, command hints, and persistent history.

### CLI

```
Usage: loomx [options]

Options:
  -work DIR       Work directory from loomc (required)
  -sv_lib NAME    User DPI shared library (without lib/.so)
  -sim BINARY     Simulation binary name (default: Vloom_shell)
  -f SCRIPT       Run commands from script file
  -s SOCKET       Socket path (default: auto PID-based)
  -timeout NS     Simulation timeout in ns
  -t TRANSPORT    Transport: socket (default) or xdma
  -d DEVICE       XDMA device path or PCI BDF (default: /dev/xdma0_user)
  --no-sim        Don't launch sim (connect to existing)
  -v              Verbose output
  -h              Show help
```

### Commands

| Command | Alias | Description |
|---------|-------|-------------|
| `run [-a] [<N>ns]` | `r` | Start emulation, service DPI loop. On first run, executes initial DPI calls and scans in the initial state image. `-a` or no args = run indefinitely. `<N>ns` or `<N>` = run for N time units. Ctrl+C interrupts back to shell. |
| `stop` | | Freeze emulation |
| `step [N]` | `s` | Step N cycles (default 1), service DPI calls during step |
| `status` | `st` | Print state, cycle count, DUT time, time compare, design info, DPI stats |
| `read <addr>` | | Read a 32-bit register at hex address. Example: `read 0x34` |
| `write <addr> <data>` | `wr` | Write a 32-bit hex value to hex address. Example: `write 0x04 0x01` |
| `dump [file.pb]` | `d` | Stop if running, scan capture, display scan data. Optionally save snapshot to protobuf file. |
| `inspect <file.pb> [var]` | | Load a saved snapshot protobuf and display metadata + variable values. Optionally filter by name prefix. |
| `deposit_script <file.pb> [out.sv]` | | Generate `$deposit` SystemVerilog statements from a snapshot. Paths come from the original HDL hierarchy. |
| `reset` | | Re-scan the initial state image and re-preload memories (scan-based reset) |
| `loadmem <mem> <file> [hex\|bin]` | `lm` | Load data file into a memory via shadow ports. Data persists across resets. Default format: hex. |
| `couple` | | Clear decoupler — connect emu_top to AXI bus |
| `decouple` | | Assert decoupler — isolate emu_top (transactions return SLVERR) |
| `help [cmd]` | `h`, `?` | List commands or show detailed help |
| `exit` | `quit`, `q` | Clean disconnect and exit |

### Interactive Example

```
$ loomx -work build/ -sv_lib dpi
[main] INFO  Loom Execution Host
[loom] INFO  Connected. Shell: 0.1.0, Hash: e3b0c44298fc1c14...
[shell] INFO  Loom interactive shell. Type 'help' for commands.
loom> status
  State:       Idle
  Cycles:      0
  DUT time:    0
  Time cmp:    unlimited
  Shell ver:   0.1.0
  Design hash: e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
  DPI funcs:   2
  Scan bits:   64
loom> step 10
[shell] INFO  Stepped 10 cycles (total: 10)
loom> run 1000ns
[shell] INFO  Scanning in initial state...
[shell] INFO  Emulation started
[shell] INFO  Emulation frozen
[shell] INFO  Cycle count: 1010
loom> run
[shell] INFO  Emulation started
^C
[shell] INFO  Interrupted
[shell] INFO  Cycle count: 2533
loom> dump snapshot.pb
  Scan chain: 64 bits (2 words)
  counter_q[31:0] = 0x0000002a
  ...
  Snapshot saved to snapshot.pb
loom> inspect snapshot.pb counter
  counter_q[31:0] = 0x0000002a
loom> exit
```

### Script Mode

Create a text file with one command per line. Lines starting with `#` are
comments.

```bash
# test_script.txt
run
exit
```

Run it:
```bash
loomx -work build/ -sv_lib dpi -f test_script.txt
```

## C++ API

### Connection

```cpp
#include "loom.h"

// Create transport and context
auto transport = loom::create_socket_transport();
loom::Context ctx(std::move(transport));

// Connect to simulation
auto rc = ctx.connect("/tmp/loom_sim.sock");
if (rc.ok()) {
    printf("Shell version: %s\n", loom::version_string(ctx.shell_version()).c_str());
    printf("Design hash: %s\n", ctx.design_hash_hex().c_str());
    printf("DPI funcs: %u\n", ctx.n_dpi_funcs());
}

// Cleanup
ctx.disconnect();
```

### Emulation Control

```cpp
// Start emulation
ctx.start();

// Get state
auto state = ctx.get_state();  // Returns Result<State>

// Get cycle count
auto cycles = ctx.get_cycle_count();  // Returns Result<uint64_t>

// Get DUT time
auto time = ctx.get_time();  // Returns Result<uint64_t>

// Set time compare (emulation freezes when time >= compare)
ctx.set_time_compare(1000);  // Run until time reaches 1000
ctx.set_time_compare(UINT64_MAX);  // Run indefinitely

// Step N cycles
ctx.step(10);

// Reset (returns to Idle state)
ctx.reset();

// Trigger shutdown with exit code
ctx.finish(0);
```

### Scan Chain

```cpp
// Capture current state
ctx.scan_capture();

// Read captured data
auto data = ctx.scan_read_data();

// Write new state and restore
ctx.scan_write_data(image);
ctx.scan_restore();
```

### Memory Shadow Access

When memories are present (`n_memories() > 0`), the host can read/write
memory contents via the shadow port controller:

```cpp
// Read a memory entry (returns n_data_words 32-bit words)
auto data = ctx.mem_read_entry(global_addr, n_data_words);

// Write a memory entry
ctx.mem_write_entry(global_addr, {0xDEADBEEF});

// Bulk preload (sequential writes with auto-increment)
ctx.mem_preload_start(base_addr, first_entry_data);
ctx.mem_preload_next(second_entry_data);
ctx.mem_preload_next(third_entry_data);
// ... continues auto-incrementing
```

Memory preload is handled transparently by the shell: on first `run` or
`step`, if a `mem_map.pb` is loaded, the shell preloads all memories with
initial content (from inline assignments or `$readmemh`/`$readmemb` files).
The `reset` command re-preloads memories alongside scan restore.

### DPI Service

```cpp
#include "loom_dpi_service.h"

// Initialize service
auto& dpi_service = loom::global_dpi_service();
dpi_service.register_funcs(loom_dpi_funcs, loom_dpi_n_funcs);

// Run blocking service loop (interrupt-driven)
loom::DpiExitCode exit_code = dpi_service.run(ctx, 30000);

// Or service one round at a time (non-blocking)
int n = dpi_service.service_once(ctx);

// Print statistics
dpi_service.print_stats();
```

### DPI Polling (Low-Level)

```cpp
// Poll for pending DPI calls (single register read)
auto pending = ctx.dpi_poll();  // Returns Result<uint32_t> bitmask

// Get call details for function 0
if (pending.value() & (1 << 0)) {
    auto call = ctx.dpi_get_call(0);  // Returns Result<DpiCall>
    // call.value().args[0], call.value().args[1], ...
}

// Complete a call with result
ctx.dpi_complete(func_id, result);
```

### Interrupt Support

```cpp
// Check if transport supports interrupts
bool has_irq = ctx.has_irq_support();

// Block until hardware interrupt fires
auto irq = ctx.wait_irq();  // Returns Result<uint32_t>
if (irq.ok()) {
    // irq.value() is the IRQ bitmask
} else if (irq.error() == loom::Error::Shutdown) {
    // Emulation ended
} else if (irq.error() == loom::Error::Interrupted) {
    // EINTR — signal received (e.g. SIGINT from Ctrl+C)
}
```

## Error Handling

All operations return `Result<T>`, a lightweight error-or-value type:

```cpp
enum class Error {
    Ok = 0,
    Transport = -1,     // Low-level I/O error
    Timeout = -2,       // Timeout waiting for operation
    InvalidArg = -3,    // Invalid argument
    NotConnected = -4,  // Not connected to target
    Protocol = -5,      // Unexpected message type
    DpiError = -6,      // DPI-specific error
    Shutdown = -7,      // Emulation ended (BFM shutdown message or EOF)
    Interrupted = -8,   // Signal received during blocking wait (EINTR)
    NotSupported = -9,  // Operation not supported by this transport
};
```

Usage pattern:

```cpp
auto result = ctx.get_state();
if (!result.ok()) {
    // Handle error based on result.error()
    return;
}
State state = result.value();
```

## Interrupt-Driven DPI Servicing

DPI function calls are serviced using an interrupt-driven architecture
with a polling fallback for transports that lack interrupt support.

### Architecture

```
DUT raises DPI call
    ↓
dpi_regfile sets pending flag → dpi_stall[i] goes high
    ↓
emu_top: irq_o[0] = |dpi_stall  (OR of all stall signals)
    ↓
BFM detects rising edge on irq_i → sends type-2 IRQ message over socket
    ↓
Host: wait_irq() returns → service_once() reads pending mask → services calls
```

### Service loop

Both `DpiService::run()` and `Shell::cmd_run()` use the same pattern:

```cpp
bool has_irq = ctx.has_irq_support();

while (!interrupted) {
    // 1. Wait for interrupt (or skip if polling)
    if (has_irq) {
        auto irq = ctx.wait_irq();
        if (irq.error() == Error::Shutdown) break;
        if (irq.error() == Error::Interrupted) continue;
    }

    // 2. Service ALL pending DPI calls
    while (true) {
        int rc = dpi_service.service_once(ctx);
        if (rc <= 0) break;  // 0 = none pending, <0 = error
    }

    // 3. Check emulation state
    auto st = ctx.get_state();
    if (st.value() == State::Frozen || st.value() == State::Error) break;

    // 4. Polling fallback (1ms backoff when no interrupt support)
    if (!has_irq && no_work_done) usleep(1000);
}
```

### DPI pending mask register

Instead of polling N individual function status registers, the host reads
a single **global pending mask** register at address `0x1_FFC0`
(func_idx=1023 in the DPI regfile):

```
Bit N = 1  →  function N has a pending call (pending && !done)
```

This supports up to 32 DPI functions per read. `dpi_poll()` is a single
`read32()` call.

## Transport Layer

The transport layer abstracts the host↔emulation communication mechanism.
Both transports implement the same `Transport` interface.

### Unix Socket Transport (Simulation)

```cpp
auto transport = loom::create_socket_transport();
```

Connects to a Verilator simulation running `loom_axil_socket_bfm`.

**Wire protocol (12-byte fixed messages):**

| Direction | Type | Description |
|-----------|------|-------------|
| Host → Sim | 0 | AXI read request (addr) |
| Host → Sim | 1 | AXI write request (addr, data) |
| Sim → Host | 0 | Read response (data, irq_bits) |
| Sim → Host | 1 | Write acknowledge (irq_bits) |
| Sim → Host | 2 | IRQ notification (irq_bits) |
| Sim → Host | 3 | SHUTDOWN (emulation ended) |

**Interrupt handling:** The BFM detects rising edges on `irq_i` and sends
type-2 messages. `wait_irq()` blocks on `recv()` until an IRQ or SHUTDOWN
message arrives. IRQs received during AXI read/write transactions are
accumulated in `pending_irq_` and returned on the next `wait_irq()` call.

**EINTR handling:** If a signal (e.g. SIGINT) interrupts `recv()` before
any data is read, `wait_irq()` returns `Error::Interrupted`. If EINTR
occurs mid-message, the read is retried to avoid data loss.

### XDMA Transport (PCIe/FPGA)

```cpp
auto transport = loom::create_xdma_transport();
loom::Context ctx(std::move(transport));
ctx.connect("/dev/xdma0_user");      // XDMA driver (pread/pwrite)
ctx.connect("0000:17:00.0");         // PCI BDF (mmap BAR0)
```

Connects to an FPGA over PCIe. Two modes:
- **XDMA driver** (`/dev/xdma0_user`) — uses `pread`/`pwrite` on the char device.
- **sysfs BAR mmap** (PCI BDF) — directly mmaps BAR0 for lowest latency.

**Interrupt handling:** Opens `/dev/xdma0_events_0` for MSI interrupt
support. `wait_irq()` blocks on `read(events_fd)` until an MSI fires
(auto-acknowledged by the kernel driver). If the events device is
unavailable, `has_irq_support()` returns false and the service loop falls
back to polling.

```bash
loomx -work build/ -t xdma                    # default /dev/xdma0_user
loomx -work build/ -t xdma -d 0000:17:00.0    # PCI BDF
```

### Transport Comparison

| Feature | Socket | XDMA (pread) | XDMA (mmap) |
|---------|--------|--------------|-------------|
| Register access | Blocking socket | pread/pwrite syscall | Direct pointer deref |
| Interrupt | Type-2 socket message | MSI via events_fd | MSI via events_fd |
| `wait_irq()` | `recv()` on socket | `read(events_fd)` | `read(events_fd)` |
| IRQ buffering | Accumulated in transport | Kernel-managed | Kernel-managed |
| `has_irq_support()` | Always true | True if events_fd open | True if events_fd open |
| Use case | Verilator simulation | FPGA (kernel driver) | FPGA (low-latency) |
