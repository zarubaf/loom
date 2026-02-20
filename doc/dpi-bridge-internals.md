<!-- SPDX-License-Identifier: Apache-2.0 -->
# DPI Bridge Internals

Implementation details for the `loom_instrument` pass and the DPI bridge
hardware/software stack. For the user-facing feature description, see
[dpi-bridge.md](dpi-bridge.md).

## Pass Pipeline

```
yosys-slang frontend          loom_instrument pass
─────────────────────          ────────────────────
DPI import + call site    →    $__loom_dpi_call cell
$display / $write         →    $print cell → $__loom_dpi_call (builtin)
$finish / $stop           →    $__loom_finish cell → loom_finish_o port
```

## `$__loom_dpi_call` Cell Attributes

Created by yosys-slang when `--loom` mode encounters a DPI import call:

| Attribute               | Example         | Description                        |
| ----------------------- | --------------- | ---------------------------------- |
| `loom_dpi_func`         | `"dpi_add"`     | C function name                    |
| `loom_dpi_arg_names`    | `"a,b"`         | Comma-separated formal names       |
| `loom_dpi_arg_types`    | `"int,int"`     | Comma-separated SV types           |
| `loom_dpi_arg_widths`   | `"32,32"`       | Comma-separated bit widths         |
| `loom_dpi_arg_dirs`     | `"input,input"` | Comma-separated directions         |
| `loom_dpi_ret_type`     | `"int"`         | Return type name                   |
| `loom_dpi_string_arg_N` | `"hello"`       | Compile-time string for arg N      |
| `loom_dpi_builtin`      | `true`          | Loom-internal (`__loom_display_*`) |
| `ARG_WIDTH`             | `64`            | Total packed input width           |
| `RET_WIDTH`             | `32`            | Return value width                 |

### Open array encoding

For `output bit [31:0] data[]` with a local 6-element array:

- `arg_types` contains `"bit[31:0]$[]"` (the `[]` suffix signals open array)
- `arg_widths` contains `"192"` (6 elements × 32 bits, inferred from call site)
- `arg_dirs` contains `"output"`

## Hardware Interface

### Bridge ports (created by `loom_instrument`)

| Port               | Dir | Width                 | Description                       |
| ------------------ | --- | --------------------- | --------------------------------- |
| `loom_dpi_valid`   | out | 1                     | DPI call pending                  |
| `loom_dpi_func_id` | out | 8                     | Function ID                       |
| `loom_dpi_args`    | out | max input width       | Packed input arguments            |
| `loom_dpi_result`  | in  | 64 + max output width | Scalar return + output array data |
| `loom_en`          | in  | 1                     | FF enable (freezes DUT when low)  |

### Argument splitting

Input and output arguments travel different hardware paths:

- **Input args** (including input open arrays) → `loom_dpi_args` (DUT → host)
- **Output open arrays** → `loom_dpi_result` (host → DUT), starting at bit 64
- **Scalar return** → `loom_dpi_result[63:0]`

The `loom_dpi_result` bus layout:

```
[63:0]              scalar return value (from RESULT_LO/HI registers)
[64+N*32-1:64]      output open array data (from host-written ARG registers)
```

### Multiple functions

When multiple DPI calls exist, the pass builds a priority-encoded mux chain:

- `loom_dpi_valid` = OR of all valid conditions
- `loom_dpi_func_id` = mux selecting active function's ID
- `loom_dpi_args` = mux selecting active function's packed input args

Each function's valid condition is derived from the cell's `EN` port
(set by yosys-slang's `set_effects_trigger`), or by tracing the
`RESULT` signal through `$pmux`/`$mux` cells.

## Emulation Wrapper (`emu_top`)

The `emu_top` pass wraps the DUT with:

- `loom_emu_ctrl` — state machine, DPI handshake, FF enable
- `loom_dpi_regfile` — per-function AXI-Lite registers
- `loom_scan_ctrl` — scan chain controller
- `loom_axil_demux` — routes host AXI-Lite to ctrl/regfile/scan

### DPI regfile register map (per function, 64 bytes)

| Offset    | Name      | R/W | Description                                                    |
| --------- | --------- | --- | -------------------------------------------------------------- |
| 0x00      | STATUS    | R   | Bit 0: pending, Bit 1: done, Bit 2: error                      |
| 0x04      | CONTROL   | W   | Bit 1: set_done, Bit 2: set_error                              |
| 0x08–0x24 | ARG0–ARG7 | R/W | Arguments (captured from DUT, host-writable for output arrays) |
| 0x28      | RESULT_LO | W   | Return value [31:0]                                            |
| 0x2C      | RESULT_HI | W   | Return value [63:32]                                           |

Address decoding: `addr[15:6]` = function index, `addr[5:0]` = register.
Base address: `0x100` (after AXI interconnect subtracts the base).

### emu_ctrl DPI state machine

```
StDpiIdle → StDpiForward → StDpiWait → StDpiComplete → StDpiIdle
```

- **StDpiForward**: Capture `func_id` and forward args to regfile
- **StDpiWait**: Wait for host to write result and set `done`
- **StDpiComplete**: Result registered; assert `dpi_ack`, release `loom_en`
  for one cycle so the DUT captures the stable registered result

The result is registered in `StDpiWait → StDpiComplete` to avoid
combinational hazards when `dpi_func_id_q` changes on the next DPI call.

## Generated Dispatch (`loom_dpi_dispatch.c`)

The `loom_instrument` pass generates a C file with:

1. **Extern declarations** — one per unique function name
2. **Per-func_id wrappers** — each creates `loom_sv_array_t` handles with
   the correct buffer size for that call site
3. **Dispatch table** — `loom_dpi_func_t` array indexed by `func_id`

### Callback signature

```c
typedef uint64_t (*loom_dpi_callback_t)(const uint32_t *args, uint32_t *out_args);
```

- `args`: packed input arguments (from hardware ARG registers)
- `out_args`: buffer for output open array data (written back to ARG registers)
- Return: scalar function return value

### Open array wrapper pattern

For each func_id, the wrapper:

1. Allocates `uint32_t buf[N]` on the stack (N = element count for this call site)
2. For input arrays: copies `args[offset..offset+N-1]` into `buf`
3. Creates `loom_sv_array_t arr = { buf, N, 32 }`
4. Calls the user function with `(svOpenArrayHandle)&arr`
5. For output arrays: copies `buf` into `out_args[0..N-1]`

### `svOpenArrayHandle` implementation

`svOpenArrayHandle` is `void*` pointing to a `loom_sv_array_t`:

```c
typedef struct {
    void *data;          // pointer to uint32_t[] element storage
    int   n_elements;    // array length
    int   elem_width;    // bits per element (32 for bit[31:0])
} loom_sv_array_t;
```

Implemented in `src/include/svdpi_openarray.c`, compiled into
`loom_dpi_dispatch.so` by `loomc`.

## Host Service Loop

### Load order (`loomx`)

1. `loom_dpi_dispatch.so` (`RTLD_LAZY | RTLD_GLOBAL`) — provides svdpi
   functions and dispatch wrappers; user function refs resolved lazily
2. User `libdpi.so` (`RTLD_NOW | RTLD_GLOBAL`) — provides DPI
   implementations; svdpi symbols resolved from dispatch lib

### `DpiService::service_once` flow

```
dpi_poll()                    → pending_mask (which functions have calls)
dpi_get_call(func_id)         → read 8 ARG registers
callback(args, out_args)      → call user function via wrapper
dpi_write_arg(func_id, i, v)  → write output array data to ARG registers
dpi_complete(func_id, result) → write RESULT_LO/HI + CONTROL(set_done)
```

## `$print` → DPI Transformation

`$display`/`$write` cells are converted to builtin DPI calls:

- Function name: `__loom_display_N`
- Format string stored as `loom_dpi_string_arg_0` (compile-time constant)
- Signal arguments packed into hardware args bus
- Generated wrapper inlines `printf()` with the format string
- No `extern` declaration (no user implementation needed)
