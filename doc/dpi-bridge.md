<!-- SPDX-License-Identifier: Apache-2.0 -->
# DPI Bridge

The `dpi_bridge` Yosys pass transforms DPI-C function calls in SystemVerilog into hardware interfaces that can be serviced by a host application.

## Overview

When yosys-slang encounters a DPI-C import declaration and call site:

```systemverilog
import "DPI-C" function int dpi_add(input int a, input int b);

// ... in always_comb block:
result = dpi_add(arg_a, arg_b);
```

It creates a `$__loom_dpi_call` cell representing the call. The `dpi_bridge` pass then:

1. Converts these cells into a multiplexed hardware interface
2. Adds control signals for the handshake protocol
3. Generates metadata (JSON) and C header for the host side

## Pass Usage

```tcl
dpi_bridge [options]

Options:
  -json_out <file>    Write DPI metadata to JSON file
  -header_out <file>  Write C function prototypes header
```

Example:
```tcl
read_slang design.sv -top my_module
proc
dpi_bridge -json_out dpi_meta.json -header_out my_module_dpi.h
```

## Hardware Interface

After transformation, the DUT module gains these ports:

| Port | Direction | Width | Description |
|------|-----------|-------|-------------|
| `loom_dpi_valid` | output | 1 | DPI call pending |
| `loom_dpi_ready` | input | 1 | Call accepted, result ready |
| `loom_dpi_func_id` | output | 8 | Function ID (0 to N-1) |
| `loom_dpi_args` | output | 64 | Packed arguments |
| `loom_dpi_result` | input | 64 | Return value from host |

### Handshake Protocol

1. DUT asserts `loom_dpi_valid` with `func_id` and `args`
2. DUT clock is gated (stalled) while waiting
3. Host reads args, executes function, writes result
4. Host asserts ready via regfile
5. DUT sees `loom_dpi_ready`, captures result, clock resumes

## Generated Files

### JSON Metadata (`dpi_meta.json`)

```json
{
  "module": "dpi_test",
  "dpi_functions": [
    {
      "id": 0,
      "name": "dpi_report_result",
      "args": [
        {"name": "arg0", "width": 32, "type": "int"},
        {"name": "arg1", "width": 32, "type": "int"}
      ],
      "return": {"width": 32, "type": "int"},
      "base_addr": "0x0100"
    },
    {
      "id": 1,
      "name": "dpi_add",
      ...
    }
  ]
}
```

### C Header (`*_dpi.h`)

```c
#ifndef DPI_TEST_DPI_H
#define DPI_TEST_DPI_H

#include <stdint.h>

// DPI function IDs
#define DPI_FUNC_DPI_REPORT_RESULT 0
#define DPI_FUNC_DPI_ADD 1

// Number of DPI functions
#define DPI_N_FUNCS 2

// DPI function prototypes - implement these in your application
int32_t dpi_report_result(int32_t arg0, int32_t arg1);
int32_t dpi_add(int32_t arg0, int32_t arg1);

#endif
```

## Multiple DPI Functions

When multiple DPI functions exist, the pass creates a multiplexed interface:

- Each function gets a unique `func_id` (assigned in order of discovery)
- Arguments are packed into a single bus
- The `loom_emu_wrapper` demultiplexes to per-function regfile slots
- Each function has dedicated argument and result registers

## Type Mapping

| SystemVerilog | C Type | Width |
|---------------|--------|-------|
| `int` | `int32_t` | 32 |
| `shortint` | `int16_t` | 16 |
| `longint` | `int64_t` | 64 |
| `byte` | `int8_t` | 8 |
| `bit [N-1:0]` | `uint32_t` / `uint64_t` | N |

## Implementation Notes

The pass handles the complexity of DPI calls appearing in combinational logic (inside `$pmux` cells from case statements). It traces through the dataflow to find unique valid conditions for each DPI function, ensuring that:

- Only one DPI function is active at a time
- The correct arguments are captured for each function
- Multi-bit valid conditions are reduced to single bits
