# CLAUDE.md — Loom: Open-Source FPGA Emulation Toolchain

## Quick Start

```bash
# Prerequisites (macOS)
brew install pkg-config libffi bison readline

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j$(nproc)

# Run tests
ctest --test-dir build --output-on-failure

# Run Yosys with plugins
./build/yosys/bin/yosys -m ./build/passes/scan_insert/scan_insert.so
```

## Implementation Notes

**Plugin linking on macOS:** Yosys plugins must NOT link against libyosys.so directly. Instead, use `-undefined dynamic_lookup` to resolve symbols at runtime from the Yosys executable. See `passes/scan_insert/CMakeLists.txt`.

**Required compile definitions for plugins:**
- `_YOSYS_`
- `YOSYS_ENABLE_PLUGINS`
- `YOSYS_ENABLE_GLOB`
- `YOSYS_ENABLE_ZLIB`

---

## Project Vision

Loom is an open-source alternative to commercial hardware emulation platforms (Zebu, Veloce, Palladium). It transforms simulation-grade SystemVerilog (including DPI-C calls) into FPGA-synthesizable RTL with scan chain instrumentation and PCIe-based host communication, targeting Xilinx/AMD FPGAs via Vivado.

**Pipeline:**
```
Source SV → yosys + loom plugins (read_slang → RTLIL → custom passes) → write_verilog → Vivado
```

---

## Repository Structure

```
loom/
├── CLAUDE.md                     # This file
├── CMakeLists.txt                # Top-level CMake build
├── cmake/
│   ├── FetchYosys.cmake          # ExternalProject_Add for Yosys
│   ├── FetchSlang.cmake          # FetchContent for slang (CMake-native)
│   └── FetchGoogleTest.cmake     # FetchContent for GoogleTest
├── passes/
│   ├── CMakeLists.txt
│   ├── scan_insert/
│   │   ├── scan_insert.cc        # Scan chain insertion pass
│   │   └── scan_insert_test.cc   # Unit tests (GoogleTest)
│   ├── dpi_bridge/
│   │   ├── dpi_bridge.cc         # DPI-to-hardware bridge pass
│   │   └── dpi_bridge_test.cc
│   └── emu_top/
│       └── emu_top.cc            # Top-level wrapper generation
├── frontend/
│   ├── CMakeLists.txt
│   └── slang_dpi_ext.cc          # Extensions to yosys-slang for DPI parsing
├── rtl/
│   ├── pcie_bridge.sv            # Parameterized PCIe bridge module
│   ├── scan_controller.sv        # Scan chain shift controller
│   └── host_mailbox.sv           # Request/response mailbox for DPI calls
├── host/
│   ├── loom_driver.h             # Host-side PCIe driver (C/C++)
│   └── dpi_server.cc             # Host process handling DPI function calls
├── tests/
│   ├── CMakeLists.txt
│   ├── fixtures/                 # Shared test SV files
│   │   ├── tiny_dff.sv           # Minimal: 1 module, 3 FFs
│   │   ├── multi_clock.sv        # Two clock domains
│   │   ├── dpi_simple.sv         # One DPI-C call
│   │   ├── dpi_multi_arg.sv      # DPI with multiple args, return value
│   │   └── hierarchy.sv          # Nested modules with FFs at multiple levels
│   ├── scan_basic/
│   │   ├── run.ys                # Yosys test script
│   │   └── check.ys             # Verification script
│   ├── scan_multiclk/
│   │   ├── run.ys
│   │   └── check.ys
│   ├── dpi_simple/
│   │   ├── run.ys
│   │   └── check.ys
│   └── e2e/
│       └── full_flow.ys          # End-to-end: read → all passes → write_verilog
├── scripts/
│   └── loom_flow.tcl             # Reference Yosys synthesis script
└── third_party/
    └── yosys-slang/              # Fork of povik/yosys-slang (git submodule)
```

---

## Build System

### Overview

CMake is the build system. All dependencies are built from source — no system installs required.

| Dependency | Version | Integration Method | Reason |
|---|---|---|---|
| **Yosys** | pinned tag (e.g. v0.58) | `ExternalProject_Add` | Uses Makefiles, can't be a CMake subdirectory |
| **slang** | pinned tag (e.g. v9.1) | `FetchContent` / `add_subdirectory` | Native CMake project |
| **yosys-slang** | our fork | `add_subdirectory` or git submodule | We modify this |
| **GoogleTest** | latest | `FetchContent` | Unit testing |

### CMake Structure

**Top-level `CMakeLists.txt`:**
```cmake
cmake_minimum_required(VERSION 3.20)
project(loom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # Required for .so plugins

# ---------- Dependencies ----------
include(cmake/FetchYosys.cmake)    # Builds Yosys, sets YOSYS_DATDIR, YOSYS_INCLUDE, YOSYS_LIB
include(cmake/FetchSlang.cmake)    # Pulls slang via FetchContent
include(cmake/FetchGoogleTest.cmake)

# ---------- Plugins ----------
add_subdirectory(third_party/yosys-slang)  # Our fork with DPI extensions
add_subdirectory(passes)
add_subdirectory(frontend)

# ---------- Tests ----------
enable_testing()
add_subdirectory(tests)
```

**`cmake/FetchYosys.cmake`:**
```cmake
include(ExternalProject)

set(YOSYS_TAG "v0.58" CACHE STRING "Yosys version to build")

ExternalProject_Add(yosys_ext
    GIT_REPOSITORY https://github.com/YosysHQ/yosys.git
    GIT_TAG        ${YOSYS_TAG}
    GIT_SHALLOW    TRUE
    PREFIX         ${CMAKE_BINARY_DIR}/yosys
    CONFIGURE_COMMAND ""
    BUILD_COMMAND   make -j${NPROC} PREFIX=<INSTALL_DIR> ENABLE_LIBYOSYS=1
    INSTALL_COMMAND make install PREFIX=<INSTALL_DIR> ENABLE_LIBYOSYS=1
    BUILD_IN_SOURCE TRUE
)

ExternalProject_Get_Property(yosys_ext INSTALL_DIR)
set(YOSYS_PREFIX   ${INSTALL_DIR} CACHE PATH "" FORCE)
set(YOSYS_INCLUDE  ${INSTALL_DIR}/share/yosys/include CACHE PATH "" FORCE)
set(YOSYS_DATDIR   ${INSTALL_DIR}/share/yosys CACHE PATH "" FORCE)
set(YOSYS_BIN      ${INSTALL_DIR}/bin/yosys CACHE FILEPATH "" FORCE)

# Import libyosys as a target
add_library(libyosys SHARED IMPORTED)
set_target_properties(libyosys PROPERTIES
    IMPORTED_LOCATION ${INSTALL_DIR}/lib/libyosys.so
    INTERFACE_INCLUDE_DIRECTORIES ${YOSYS_INCLUDE}
)
add_dependencies(libyosys yosys_ext)
```

**`cmake/FetchSlang.cmake`:**
```cmake
include(FetchContent)

set(SLANG_TAG "v9.1" CACHE STRING "slang version")

FetchContent_Declare(slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG        ${SLANG_TAG}
    GIT_SHALLOW    TRUE
)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(slang)
```

**`cmake/FetchGoogleTest.cmake`:**
```cmake
include(FetchContent)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)
```

**`passes/CMakeLists.txt` (example for scan_insert):**
```cmake
# Plugin shared library
add_library(scan_insert MODULE scan_insert/scan_insert.cc)
target_link_libraries(scan_insert PRIVATE libyosys)
set_target_properties(scan_insert PROPERTIES PREFIX "" SUFFIX ".so")

# Unit tests
add_executable(scan_insert_test scan_insert/scan_insert_test.cc)
target_link_libraries(scan_insert_test PRIVATE libyosys GTest::gtest_main)
gtest_discover_tests(scan_insert_test)
```

### Building

```bash
git clone --recursive https://github.com/your-org/loom.git
cd loom
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

---

## Testing Strategy

### Tier 1: C++ Unit Tests (GoogleTest + CTest)

Test RTLIL manipulation logic in isolation. These are fast (milliseconds) and run on every build.

```cpp
// scan_insert_test.cc
#include <gtest/gtest.h>
#include "kernel/yosys.h"

USING_YOSYS_NAMESPACE

class ScanInsertTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Yosys must be initialized once
        yosys_setup();
        design = new RTLIL::Design;
    }
    void TearDown() override {
        delete design;
        yosys_shutdown();
    }
    RTLIL::Design *design;

    // Helper: create a module with N simple DFFs
    RTLIL::Module* make_module_with_dffs(const std::string &name, int n_dffs) {
        auto mod = design->addModule(RTLIL::escape_id(name));
        auto clk = mod->addWire(ID(\\clk), 1);
        clk->port_input = true;

        for (int i = 0; i < n_dffs; i++) {
            auto d = mod->addWire(NEW_ID, 1);
            auto q = mod->addWire(NEW_ID, 1);
            auto cell = mod->addDff(NEW_ID, clk, d, q);
            // cell is a $dff with CLK, D, Q ports
        }
        mod->fixup_ports();
        return mod;
    }
};

TEST_F(ScanInsertTest, FindsAllDFFs) {
    auto mod = make_module_with_dffs("\\test", 5);
    int count = 0;
    for (auto cell : mod->cells())
        if (cell->type == ID($dff)) count++;
    EXPECT_EQ(count, 5);
}

TEST_F(ScanInsertTest, InsertsMuxPerDFF) {
    auto mod = make_module_with_dffs("\\test2", 3);
    // Run scan insertion logic here (call your function directly)
    // ...
    int mux_count = 0;
    for (auto cell : mod->cells())
        if (cell->type == ID($mux)) mux_count++;
    EXPECT_EQ(mux_count, 3);  // One mux per DFF
}

TEST_F(ScanInsertTest, AddsPortsCorrectly) {
    auto mod = make_module_with_dffs("\\test3", 2);
    // Run scan insertion
    // ...
    EXPECT_NE(mod->wire(ID(\\scan_enable)), nullptr);
    EXPECT_NE(mod->wire(ID(\\scan_in)), nullptr);
    EXPECT_NE(mod->wire(ID(\\scan_out)), nullptr);
    EXPECT_TRUE(mod->wire(ID(\\scan_enable))->port_input);
    EXPECT_TRUE(mod->wire(ID(\\scan_out))->port_output);
}
```

### Tier 2: Yosys Script Integration Tests (CTest)

These load real SV, run the full pass pipeline, and check properties.

**`tests/scan_basic/run.ys`:**
```tcl
# Load plugins
plugin -i slang
plugin -i scan_insert

# Read design
read_slang ../fixtures/tiny_dff.sv --top tiny_dff

# Elaborate
proc
opt_clean

# Count FFs before
log "=== Before scan insertion ==="
select t:$dff
stat

# Insert scan chain
scan_insert

# Verify
log "=== After scan insertion ==="

# Check: scan ports exist
select w:scan_enable
stat
# This should report 1 wire

# Check: mux count equals DFF count
select t:$mux
stat

# Check: design is still valid
check -assert

# Write output
write_verilog scan_basic_output.v
```

**`tests/CMakeLists.txt`:**
```cmake
# Helper function for Yosys script tests
function(add_yosys_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${CMAKE_BINARY_DIR}/third_party/yosys-slang/slang.so
            -m ${CMAKE_BINARY_DIR}/passes/scan_insert.so
            -m ${CMAKE_BINARY_DIR}/passes/dpi_bridge.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;scan_insert;dpi_bridge"
        TIMEOUT 60
    )
endfunction()

# Register all Yosys script tests
add_yosys_test(scan_basic)
add_yosys_test(scan_multiclk)
add_yosys_test(dpi_simple)
add_yosys_test(e2e)
```

### Tier 3: Vivado Synthesis Tests (Optional / Nightly)

```cmake
# Only if Vivado is available
find_program(VIVADO vivado)
if(VIVADO)
    add_test(
        NAME vivado_synth_basic
        COMMAND ${VIVADO} -mode batch -source synth_check.tcl
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/e2e
    )
    set_tests_properties(vivado_synth_basic PROPERTIES
        LABELS "slow;vivado"
        TIMEOUT 600
    )
endif()
```

Run only Vivado tests: `ctest --test-dir build -L vivado`

### Test Fixtures

**`tests/fixtures/tiny_dff.sv`:**
```systemverilog
module tiny_dff (
    input  logic        clk,
    input  logic        rst,
    input  logic [7:0]  data_in,
    output logic [7:0]  data_out,
    output logic        flag
);
    logic [7:0] reg_a;
    logic       reg_b;

    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            reg_a <= 8'h0;
            reg_b <= 1'b0;
        end else begin
            reg_a <= data_in;
            reg_b <= |data_in;
        end
    end

    assign data_out = reg_a;
    assign flag = reg_b;
endmodule
```

**`tests/fixtures/dpi_simple.sv`:**
```systemverilog
import "DPI-C" function int compute_hash(input int data, input int seed);

module dpi_simple (
    input  logic        clk,
    input  logic        rst,
    input  logic [31:0] data_in,
    output logic [31:0] hash_out
);
    always_ff @(posedge clk) begin
        if (rst)
            hash_out <= 32'h0;
        else
            hash_out <= compute_hash(data_in, 32'hDEADBEEF);
    end
endmodule
```

---

## Part 1: Yosys Internals & RTLIL API

### Core Data Structures

All Yosys passes operate on RTLIL (RTL Intermediate Language). The key types:

```
RTLIL::Design      — Root container, holds modules
  └── RTLIL::Module   — A single module (like a Verilog module)
       ├── RTLIL::Wire    — A signal (may be multi-bit, has width/offset/direction)
       ├── RTLIL::Cell    — An instance of a cell type (primitive or submodule)
       └── RTLIL::Process — Behavioral code (from always blocks) — temporary, removed by `proc`
```

**Naming convention:** All RTLIL identifiers start with `\` (user-visible) or `$` (auto-generated internal). E.g., `\clk`, `\data_in`, `$auto$proc.cc:123$45`.

### Key Types

```cpp
RTLIL::IdString   // Interned string handle. Lightweight (single int).
                  // Create with: ID(\my_wire), ID($dff), or module->uniquify("\\_scan_")
RTLIL::SigBit     // A single bit: either a constant (0,1,x,z) or one bit of a wire
RTLIL::SigSpec    // A vector of SigBits — the universal "signal" type
RTLIL::Const      // A constant value (bitvector)
RTLIL::Wire       // A wire/reg declaration with width, port info, attributes
RTLIL::Cell       // A cell instance with type, parameters, and port connections
```

### Common Patterns

**Creating a wire:**
```cpp
RTLIL::Wire *w = module->addWire(ID(\scan_enable), 1);
w->port_input = true;
module->fixup_ports();  // Always call after changing port properties
```

**Creating a cell:**
```cpp
RTLIL::Cell *mux = module->addMux(NEW_ID, sig_a, sig_b, sig_sel, sig_y);
```

**Iterating cells:**
```cpp
for (auto cell : module->cells()) {
    if (cell->type == ID($dff)) {
        RTLIL::SigSpec clk = cell->getPort(ID::CLK);
        RTLIL::SigSpec d   = cell->getPort(ID::D);
        RTLIL::SigSpec q   = cell->getPort(ID::Q);
        int width = cell->getParam(ID::WIDTH).as_int();
    }
}
```

**SigMap for signal canonicalization:**
```cpp
SigMap sigmap(module);
RTLIL::SigSpec canonical = sigmap(some_signal);
```

### Flip-Flop Cell Types (Critical for Scan Insertion)

After the `proc` pass, all flip-flops become explicit cells:

| Cell Type | Ports | Description |
|-----------|-------|-------------|
| `$dff` | `\CLK`, `\D`, `\Q` | Basic D flip-flop |
| `$dffe` | + `\EN` | With enable |
| `$adff` | + `\ARST` | With async reset |
| `$adffe` | + `\ARST`, `\EN` | With async reset + enable |
| `$sdff` | + `\SRST` | With sync reset |
| `$sdffe` | + `\SRST`, `\EN` | With sync reset + enable (reset over enable) |
| `$sdffce` | + `\SRST`, `\EN` | With sync reset + enable (enable over reset) |
| `$dffsr` | + `\SET`, `\CLR` | With async set and reset |
| `$dffsre` | + `\SET`, `\CLR`, `\EN` | With async set/reset + enable |
| `$aldff` | + `\ALOAD`, `\AD` | With async load |

**Common parameters:** `WIDTH`, `CLK_POLARITY` (1'b1 = posedge).

**Helper:**
```cpp
bool is_ff(RTLIL::Cell *cell) {
    return cell->type.in(
        ID($dff), ID($dffe), ID($adff), ID($adffe),
        ID($sdff), ID($sdffe), ID($sdffce),
        ID($dffsr), ID($dffsre), ID($aldff), ID($aldffe)
    );
}
```

---

## Part 2: Writing a Yosys Pass

### Pass Template

```cpp
#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ScanInsertPass : public Pass {
    ScanInsertPass() : Pass("scan_insert", "Insert scan chains into the design") {}

    void help() override {
        log("\n");
        log("    scan_insert [options] [selection]\n");
        log("\n");
        log("Insert scan chain multiplexers on all flip-flops.\n");
        log("\n");
        log("    -chain_length N\n");
        log("        Maximum flip-flops per chain (default: all in one chain)\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing SCAN_INSERT pass.\n");

        int chain_length = 0;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-chain_length" && argidx + 1 < args.size()) {
                chain_length = atoi(args[++argidx].c_str());
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));
            run_scan_insert(module, chain_length);
        }
    }

    void run_scan_insert(RTLIL::Module *module, int chain_length) {
        std::vector<RTLIL::Cell*> dffs;
        for (auto cell : module->cells())
            if (is_ff(cell))
                dffs.push_back(cell);

        if (dffs.empty()) return;
        log("  Found %zu flip-flops\n", dffs.size());

        RTLIL::Wire *scan_en = module->addWire(ID(\scan_enable), 1);
        scan_en->port_input = true;
        RTLIL::Wire *scan_in = module->addWire(ID(\scan_in), 1);
        scan_in->port_input = true;
        RTLIL::Wire *scan_out = module->addWire(ID(\scan_out), 1);
        scan_out->port_output = true;

        RTLIL::SigSpec prev_q = RTLIL::SigSpec(scan_in);

        for (auto dff : dffs) {
            RTLIL::SigSpec orig_d = dff->getPort(ID::D);
            RTLIL::SigSpec q = dff->getPort(ID::Q);
            int width = dff->getParam(ID::WIDTH).as_int();

            RTLIL::SigSpec mux_out = module->addWire(NEW_ID, width);
            module->addMux(NEW_ID, orig_d, prev_q, scan_en, mux_out);
            dff->setPort(ID::D, mux_out);
            prev_q = q;
        }

        module->connect(RTLIL::SigSpec(scan_out), RTLIL::SigBit(prev_q[0]));
        module->fixup_ports();
        log("  Inserted scan chain with %zu elements\n", dffs.size());
    }

    bool is_ff(RTLIL::Cell *cell) {
        return cell->type.in(
            ID($dff), ID($dffe), ID($adff), ID($adffe),
            ID($sdff), ID($sdffe), ID($sdffce),
            ID($dffsr), ID($dffsre), ID($aldff), ID($aldffe)
        );
    }
};

ScanInsertPass ScanInsertPass;

PRIVATE_NAMESPACE_END
```

### Key API Patterns

**Adding a module:**
```cpp
RTLIL::Module *bridge = design->addModule(ID(\dpi_bridge));
bridge->fixup_ports();
```

**Replacing a cell:**
```cpp
module->remove(old_cell);
RTLIL::Cell *inst = module->addCell(NEW_ID, ID(\dpi_bridge));
inst->setPort(ID(\clk), clk_sig);
inst->setParam(ID(\DATA_WIDTH), data_width);
```

**Connecting wires:**
```cpp
module->connect(dest_sigspec, src_sigspec);
```

---

## Part 3: yosys-slang Architecture

### Key Files in `src/`

- **`slang_frontend.cc`** — `read_slang` command entry point
- **`slang_frontend.h`** — Shared state
- **`builder.h` / `builder.cc`** — Core AST→RTLIL translation (`NetlistContext`, `SignalEvalContext`)
- **`addressing.cc`** — Array/struct addressing, bit slicing
- **`initial_eval.cc`** — `initial` block evaluation
- **`diag.cc`** — Diagnostics

### Frontend Flow

1. `read_slang` creates a `slang::ast::Compilation` from source files
2. Walks top-level instances
3. For each instance, builder creates `RTLIL::Module`
4. Visits AST nodes → creates RTLIL wires, cells, processes
5. Unsupported constructs (like DPI) currently error

### Where to Hook DPI Handling

DPI declarations in slang's AST are `slang::ast::SubroutineSymbol` nodes with DPI attributes. Call sites are `slang::ast::CallExpression` nodes.

**Strategy:**
1. Detect `import "DPI-C"` declarations during AST walking
2. Record signature (name, arg types, return type, widths)
3. At call sites, emit instantiation of `\loom_dpi_bridge_<funcname>` cell
4. After frontend completes, generate bridge module definitions in RTLIL

**Key slang types:**
```cpp
slang::ast::SubroutineSymbol     // Function/task declaration
slang::ast::CallExpression       // Function call site
slang::ast::FormalArgumentSymbol // Function parameter
slang::ast::Type                 // Resolved type (has getBitstreamWidth())
```

---

## Part 4: DPI Bridge Architecture

```
DUT side:                          Host side (PCIe):
┌──────────────┐  AXI-Lite/MM  ┌──────────────┐
│loom_dpi_bridge├──────────────┤ XDMA/QDMA IP │── PCIe ── Host CPU
│ (per function)│  (mailbox)    └──────────────┘    running loom_server
└──────────────┘
```

For `int foo(int a, logic [31:0] b)`:

**Hardware ports:** `clk`, `rst`, `call_valid`/`call_ready`, `arg_0[31:0]`, `arg_1[31:0]`, `ret_valid`/`ret_ready`, `ret_data[31:0]`, AXI interface.

**Protocol:**
1. DUT writes args, asserts `call_valid`
2. Bridge writes to PCIe-accessible registers, signals host
3. Host reads args, executes C function, writes return
4. Bridge asserts `ret_valid`
5. DUT reads return, continues

**Stalling:** The calling block must stall while waiting. The bridge's `stall` output connects to the relevant clock enable.

---

## Part 5: Scan Chain Pass Details

### Algorithm (runs after `proc`)

1. **Collect** all `$dff*` cells
2. **Partition** by clock domain or fixed chain length
3. **Per chain:**
   a. Add `scan_enable`, `scan_in`, `scan_out` ports
   b. Per FF: create `$mux` (sel=scan_enable, A=orig_D, B=prev_Q), reconnect FF's `\D`
   c. Wire last Q to `scan_out`
4. **Add** scan controller module
5. **`fixup_ports()`**

### Width Handling

Multi-bit FFs need serial or parallel scan. For FPGA emulation, parallel scan (matching FF width) is fine — routing is less constrained than ASIC.

---

## Part 6: Reference Yosys Flow

```tcl
# loom_flow.tcl
plugin -i loom_slang
plugin -i loom_passes

read_slang design.sv --top my_soc -I include/ --allow-dpi-bridge
proc
flatten
opt

scan_insert -chain_length 4096
dpi_bridge_elaborate -pcie_ip xdma -base_addr 0x10000
emu_top -board xcu200 -pcie xdma

write_verilog -noattr loom_output.v
```

---

## Part 7: Key Reference Files

**In Yosys source:**

| File | Why |
|------|-----|
| `passes/opt/opt_dff.cc` | DFF manipulation patterns |
| `passes/techmap/dff2dffe.cc` | DFF transformation |
| `passes/cmds/stat.cc` | Simple pass example |
| `kernel/rtlil.h` | All RTLIL types |
| `kernel/sigtools.h` | `SigMap` and signal utilities |
| `kernel/register.h` | `Pass`, `Frontend`, `Backend` base classes |
| `kernel/celltypes.h` | Cell type definitions |
| `backends/verilog/verilog_backend.cc` | `write_verilog` implementation |

**In yosys-slang:**

| File | Why |
|------|-----|
| `src/slang_frontend.cc` | Entry point, Compilation setup |
| `src/builder.h` | Core AST→RTLIL mapping |
| `src/builder.cc` | Statement/expression handling |

---

## Coding Conventions

- Use `log()` / `log_header()` / `log_warning()` / `log_error()`, never `printf`
- Use `NEW_ID` for auto-generated names, `ID(\name)` for user-visible, `ID($type)` for cell types
- Always call `module->fixup_ports()` after modifying ports
- Use `log_id(thing)` to print names in log messages
- Wrap pass internals in `PRIVATE_NAMESPACE_BEGIN` / `PRIVATE_NAMESPACE_END`
- Static `Pass` object auto-registers with Yosys

---

## Agent Task Breakdown

### Agent 1: yosys-slang DPI Extension (`frontend/`)
**Goal:** Modify AST walker to handle DPI declarations and call sites.
**Test:** `read_slang dpi_simple.sv` completes, placeholder cells visible in `dump`.

### Agent 2: Scan Chain Pass (`passes/scan_insert/`)
**Goal:** Yosys pass inserting scan muxes on all DFFs.
**Test:** Unit tests (GoogleTest) + Yosys script tests verifying mux count, port creation, valid output.

### Agent 3: PCIe Bridge RTL & Host Driver (`rtl/` + `host/`)
**Goal:** Parameterized bridge SV modules + C++ host-side DPI server.
**Test:** Simulation testbench for request/response cycle.

### Integration
1. All plugins loaded together
2. E2E: SV with DPI → Yosys flow → Vivado-synthesizable Verilog
3. FPGA bitstream + host-side smoke test
