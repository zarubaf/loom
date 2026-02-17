<!-- SPDX-License-Identifier: Apache-2.0 -->
# Host Library

The Loom host library provides a C++ API for communicating with an emulated design, servicing DPI function calls, and an interactive shell for debugging.

## Components

```
src/host/
├── loom.h                    # Main API header (Context, Transport)
├── loom.cpp                  # Core library implementation
├── loom_transport_socket.cpp # Unix socket transport
├── loom_dpi_service.h/cpp    # Generic DPI service loop
├── loom_shell.h/cpp          # Interactive shell (replxx-based)
├── loom_sim_main.cpp         # Main entry point
├── loom_vpi.cpp              # VPI implementation ($finish/$stop)
└── loom_log.h                # Header-only logging
```

## Interactive Shell

The Loom host binary starts an interactive shell by default. The shell provides tab completion, command hints, syntax highlighting, and persistent history.

### CLI

```
Usage: loom_host [options] [socket_path]
  -f <script>   Execute commands from script file
  -v            Verbose (debug logging)
  -h            Show help
Default socket: /tmp/loom_sim.sock
```

### Commands

| Command | Alias | Description |
|---------|-------|-------------|
| `run [-a] [<N>ns]` | `r` | Release DUT reset, start emulation, service DPI loop. `-a` or no args = run indefinitely. `<N>ns` or `<N>` = run for N time units from current time. Ctrl+C interrupts back to shell. |
| `stop` | | Freeze emulation |
| `step [N]` | `s` | Step N cycles (default 1), service DPI calls during step |
| `status` | `st` | Print state, cycle count, DUT time, time compare, design info, DPI stats |
| `dump` | `d` | Stop if running, scan capture, display scan data |
| `reset` | | Assert DUT reset |
| `help [cmd]` | `h`, `?` | List commands or show detailed help |
| `exit` | `quit`, `q` | Clean disconnect and exit |

### Interactive Example

```
$ loom_host /tmp/loom_sim.sock
[main] INFO  Loom Simulation Host
[loom] INFO  Connected. Design ID: 0x00000001 ...
[shell] INFO  Loom interactive shell. Type 'help' for commands.
loom> status
  State:       Idle
  Cycles:      0
  DUT time:    0
  Time cmp:    unlimited
  Design ID:   0x00000001
  DPI funcs:   2
  Scan bits:   64
loom> step 10
[shell] INFO  Stepped 10 cycles (total: 10)
loom> run 1000ns
[shell] INFO  Emulation started
[shell] INFO  Emulation frozen
[shell] INFO  Cycle count: 1010
[shell] INFO  DUT time: 1010
loom> run
[shell] INFO  Emulation started
^C
[shell] INFO  Interrupted
[shell] INFO  Cycle count: 2533
[shell] INFO  DUT time: 2533
loom> dump
  Scan chain: 64 bits (2 words)
  [ 0] 0x0000002a
  [ 1] 0x00000000
loom> exit
```

### Script Mode

Create a text file with one command per line. Lines starting with `#` are comments.

```bash
# test_script.txt
run
exit
```

Run it:
```bash
loom_host -f test_script.txt /tmp/loom_sim.sock
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
    printf("Design ID: 0x%08x\n", ctx.design_id());
    printf("DPI funcs: %u\n", ctx.n_dpi_funcs());
}

// Cleanup
ctx.disconnect();
```

### Emulation Control

```cpp
// Release DUT reset
ctx.dut_reset(false);

// Start emulation
ctx.start();

// Get state
auto state = ctx.get_state();  // Returns Result<State>

// Get cycle count
auto cycles = ctx.get_cycle_count();  // Returns Result<uint64_t>

// Get DUT time
auto time = ctx.get_time();  // Returns Result<uint64_t>

// Set time compare (emulation freezes when time >= compare)
ctx.set_time_compare(1000);  // Run for 1000 time units from 0
ctx.set_time_compare(UINT64_MAX);  // Run indefinitely

// Get current time compare value
auto cmp = ctx.get_time_compare();  // Returns Result<uint64_t>
```

### DPI Service

```cpp
#include "loom_dpi_service.h"

// Initialize service
auto& dpi_service = loom::global_dpi_service();
dpi_service.register_funcs(loom_dpi_funcs, loom_dpi_n_funcs);

// Run blocking service loop
loom::DpiExitCode exit_code = dpi_service.run(ctx, 30000);

// Or service one round at a time (non-blocking)
int n = dpi_service.service_once(ctx);

// Print statistics
dpi_service.print_stats();
```

### DPI Polling (Low-Level)

```cpp
// Poll for pending DPI calls
auto pending = ctx.dpi_poll();  // Returns Result<uint32_t> bitmask

// Get call details for function 0
if (pending.value() & (1 << 0)) {
    auto call = ctx.dpi_get_call(0);  // Returns Result<DpiCall>
    // call.value().args[0], call.value().args[1], ...
}

// Complete a call with result
ctx.dpi_complete(func_id, result);
```

## DPI Service Library

For typical use cases, the generic DPI service handles the polling loop automatically.

### Setup

```cpp
#include "loom_dpi_service.h"
#include "my_design_dpi.h"  // Generated header

// Implement the DPI functions
int32_t dpi_add(int32_t a, int32_t b) {
    return a + b;
}

// Wrapper callbacks (adapt typed functions to generic signature)
static uint64_t wrap_dpi_add(const uint32_t *args) {
    return (uint64_t)dpi_add((int32_t)args[0], (int32_t)args[1]);
}

// Function table
static const loom_dpi_func_t dpi_funcs[] = {
    { DPI_FUNC_DPI_ADD, "dpi_add", 2, 32, wrap_dpi_add },
};
```

### Exit Conditions

The service loop exits when:

1. Emulation enters FROZEN or ERROR state
2. Shutdown message received from simulation ($finish/$stop)
3. Timeout expires with no DPI activity

## Transport Layer

The transport layer abstracts the communication mechanism. Currently supported:

### Unix Socket Transport

```cpp
auto transport = loom::create_socket_transport();
// ... use with Context ...
```

The socket transport connects to a Verilator simulation running `loom_axil_socket_bfm`.

### Future Transports

- PCIe (XDMA/QDMA) for FPGA deployment
- Shared memory for fast local simulation
