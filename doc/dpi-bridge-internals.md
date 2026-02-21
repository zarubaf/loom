<!-- SPDX-License-Identifier: Apache-2.0 -->
# DPI Bridge Internals

Implementation details for the `loom_instrument` pass and the DPI bridge
hardware/software stack.

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

### Fixed-size array encoding

For `input bit [31:0] data[4]`:

- `arg_types` contains `"bit[31:0]$[4]"` (the `[N]` suffix signals fixed array)
- `arg_widths` contains `"128"` (4 elements × 32 bits)
- `arg_dirs` contains `"input"`

Fixed-size arrays are passed identically to open arrays in hardware (packed
into the args bus). The only difference is in the generated dispatch
wrapper, which uses the compile-time known size.

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
Base address: `0x10000` in the overall address map (via `loom_axil_demux`).

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
dpi_poll()                    → pending_mask (single register read at 0x1_FFC0)
for each set bit i:
  dpi_get_call(i)             → read ARG registers
  callback(args, out_args)    → call user function via dispatch wrapper
  dpi_write_arg(i, j, v)     → write output array data to ARG registers
  dpi_complete(i, result)     → write RESULT_LO/HI + CONTROL(set_done)
```

The pending mask at func_idx=1023 returns one bit per function (bit N =
function N pending && !done), allowing a single AXI read to determine
which functions need servicing.

### Interrupt-driven servicing

The service loop is interrupt-driven: the host blocks on `wait_irq()`
until the hardware signals a DPI call is pending, then services all
pending calls in a tight loop. See [host-library.md](host-library.md)
for the full service loop architecture and transport details.

## Initial and Reset DPI Calls

DPI calls can appear in two non-runtime contexts that Loom handles specially:

### Void DPI calls in `initial` blocks

```systemverilog
initial begin
    init_setup("config.txt");  // side-effect only, no return value used
end
```

These execute at simulation t=0 for side effects (opening files, initializing
state). The frontend (`initial_eval.cc`) captures these during constant
evaluation, recording the function name and compile-time constant arguments.
A `$__loom_dpi_call` cell is created with `loom_dpi_initial=true`.

**Pipeline:**
- `scan_insert` records a `DpiInitCall` entry (return_width=0) in `scan_map.pb`
- `loom_instrument` adds the function to the dispatch table but creates no
  hardware bridge (the cell is removed)
- At runtime, the host executes the call before scanning in the initial image

### DPI calls in reset blocks

```systemverilog
always_ff @(posedge clk_i or negedge rst_ni)
    if (!rst_ni) reg_q <= get_init_val(42);
    else         reg_q <= reg_q + 1;
```

These provide DPI-computed initial values for registers. Yosys represents them
as `$aldff`/`$aldffe` cells where the AD (async data) port is driven by a
DPI call result.

**Pipeline:**
- `reset_extract` traces the AD port to the `$__loom_dpi_call` cell, marks it
  with `loom_dpi_reset=true`, stores `loom_reset_dpi_func` on the Q wire, and
  strips the cell to `$dff`/`$dffe`
- `scan_insert` records a `DpiInitCall` with the FF's scan chain offset and width
- `loom_instrument` adds the function to the dispatch table (no hardware bridge)
- At runtime, the host calls the DPI function, then patches the return value
  into the scan image at the recorded bit offset before scanning in

### Error case: DPI with used result in `initial` block

```systemverilog
initial begin
    x = get_value();  // ERROR: result assigned to variable
end
```

This is unsupported because there is no scan chain target for the return value
(unlike the reset block case where the target FF is known). The frontend
produces an error: "DPI call with used result in initial block is not supported."

### `DpiInitCall` protobuf message

Stored in `ScanMap.initial_dpi_calls`:

| Field          | Type              | Description                              |
| -------------- | ----------------- | ---------------------------------------- |
| `func_name`    | string            | DPI function name (dispatch table key)   |
| `arg_data`     | bytes             | LE-packed constant arguments             |
| `arg_widths`   | string            | Comma-separated per-arg bit widths       |
| `arg_types`    | string            | Comma-separated arg types                |
| `arg_dirs`     | string            | Comma-separated arg directions           |
| `return_width` | uint32            | 0 for void initial calls                 |
| `scan_offset`  | uint32            | Bit offset in scan chain (reset DPI)     |
| `scan_width`   | uint32            | Width of return value in scan chain      |
| `string_args`  | map<uint32,string>| Index → compile-time string value        |

## `$print` → DPI Transformation

`$display`/`$write` cells are converted to builtin DPI calls:

- Function name: `__loom_display_N`
- Format string stored as `loom_dpi_string_arg_0` (compile-time constant)
- Signal arguments packed into hardware args bus
- Generated wrapper inlines `printf()` with the format string
- No `extern` declaration (no user implementation needed)
