# Loom Development Plan

## Overview

Staged approach to building the Loom FPGA emulation toolchain. Each stage ends with a working, committable state.

---

## Stage 1: Basic Repository Setup
**Commit message:** `Initial repository structure and CMake skeleton`

### Tasks:
1. Initialize git repository
2. Create directory structure per CLAUDE.md
3. Create top-level `CMakeLists.txt` (skeleton)
4. Create `cmake/` modules:
   - `FetchYosys.cmake`
   - `FetchSlang.cmake`
   - `FetchGoogleTest.cmake`
5. Create placeholder `CMakeLists.txt` in subdirectories
6. Add `.gitignore` for build artifacts

### Deliverables:
- Running `cmake -B build` should configure (even if dependencies fail to fetch initially)
- Clean directory structure

---

## Stage 2: Yosys Dependency Building
**Commit message:** `Add Yosys external project build`

### Tasks:
1. Complete `FetchYosys.cmake` with ExternalProject_Add
2. Handle platform differences (macOS vs Linux)
3. Verify Yosys builds and libyosys.so/dylib is produced
4. Create imported target for libyosys

### Verification:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target yosys_ext
# Should produce build/yosys/bin/yosys
```

---

## Stage 3: Minimal scan_insert Pass
**Commit message:** `Add skeleton scan_insert pass that loads into Yosys`

### Tasks:
1. Create `passes/scan_insert/scan_insert.cc` with minimal Pass structure
2. Create `passes/CMakeLists.txt` to build as MODULE
3. Verify plugin loads: `yosys -m scan_insert.so -p "help scan_insert"`

### Code skeleton:
```cpp
// Minimal pass that just prints a message
struct ScanInsertPass : public Pass {
    ScanInsertPass() : Pass("scan_insert", "Insert scan chains") {}
    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log("scan_insert: Not yet implemented\n");
    }
};
```

---

## Stage 4: Test Infrastructure
**Commit message:** `Add test infrastructure with GoogleTest and fixtures`

### Tasks:
1. Create `tests/CMakeLists.txt` with helper functions
2. Create test fixtures:
   - `tests/fixtures/tiny_dff.sv`
   - `tests/fixtures/hierarchy.sv`
3. Create GoogleTest skeleton for scan_insert
4. Verify `ctest --test-dir build` runs (even if tests are minimal)

---

## Stage 5: scan_insert Implementation
**Commit message:** `Implement scan chain insertion pass`

### Tasks:
1. Implement FF detection (`is_ff()` helper)
2. Implement scan port creation
3. Implement mux insertion per FF
4. Implement chain wiring
5. Add unit tests for each component
6. Add Yosys script integration tests

### Sub-commits possible:
- `scan_insert: Add FF detection logic`
- `scan_insert: Add scan port creation`
- `scan_insert: Add mux insertion`
- `scan_insert: Add integration tests`

---

## Stage 6: yosys-slang Integration
**Commit message:** `Add yosys-slang SystemVerilog frontend`

### Overview
Integrate povik/yosys-slang as a git submodule in `third_party/yosys-slang/`. This provides `read_slang` command for IEEE 1800-2017/2023 SystemVerilog support.

### Tasks:
1. Add yosys-slang as git submodule in `third_party/`
2. Create `cmake/FetchYosysSlang.cmake` to build the plugin
3. Update top-level CMake to include yosys-slang build
4. Verify `read_slang` loads and works with test fixtures
5. Create end-to-end test: SV → read_slang → proc → scan_insert → write_verilog

### Sub-stages:
- **6a**: Add submodule and basic CMake integration
- **6b**: Verify read_slang works with tiny_dff.sv
- **6c**: Add end-to-end test

### Build considerations:
- yosys-slang requires: slang library, {fmt}
- Plugin output: `slang.so`
- Must use same `-undefined dynamic_lookup` pattern as scan_insert on macOS

---

## Stage 7: Sequential Equivalence Checking
**Commit message:** `Add equivalence checking for scan chain insertion`

### Overview
Add `-check_equiv` option to scan_insert that verifies the design is functionally equivalent before and after scan insertion when scan ports are tied off (scan_enable=0, scan_in=0).

### Tasks:
1. Add `-check_equiv` flag to scan_insert pass
2. Before insertion: save copy of original design
3. After insertion: tie off scan ports (scan_enable=0)
4. Run Yosys equiv_* passes to verify equivalence
5. Report PASS/FAIL result

### Implementation approach:
```tcl
# Conceptual flow (inside scan_insert pass):
# 1. Copy design to "gold"
# 2. Insert scan chain in "gate"
# 3. Tie off scan ports in "gate": scan_enable=0, scan_in=0
# 4. Run: equiv_make gold gate equiv
# 5. Run: equiv_induct equiv
# 6. Run: equiv_status -assert
```

### Yosys equiv commands to use:
- `equiv_make` - Create equivalence checking module
- `equiv_simple` - Simple combinational equivalence
- `equiv_induct` - Inductive equivalence for sequential circuits
- `equiv_status` - Check equivalence status

### Test:
- Run on tiny_dff.sv with `-check_equiv`
- Should pass (scan chain is functionally transparent when disabled)

---

## Stage 8: DPI Bridge Pass (Future)
**Commit message:** `Add DPI bridge pass skeleton`

### Overview
Fork yosys-slang to handle DPI-C declarations. Instead of erroring on `import "DPI-C"`, capture the signature and generate placeholder cells.

### Tasks:
1. Fork yosys-slang to `third_party/yosys-slang/` (or use our submodule)
2. Modify slang frontend to detect DPI imports
3. Store DPI function signatures as RTLIL attributes
4. Create `passes/dpi_bridge/dpi_bridge.cc` pass
5. Generate bridge module instantiations from DPI attributes

### DPI handling strategy:
- DPI import → create `$loom_dpi_call` cell with attributes for function name, args
- DPI export → not supported initially (error)
- Bridge pass converts `$loom_dpi_call` to hardware mailbox interface

---

## Stage 9: RTL Components (Future)
**Commit message:** `Add PCIe bridge RTL modules`

### Tasks:
1. Create `rtl/pcie_bridge.sv`
2. Create `rtl/scan_controller.sv`
3. Create `rtl/host_mailbox.sv`

---

## Development Workflow

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build everything
cmake --build build -j$(nproc)

# Run all tests
ctest --test-dir build --output-on-failure

# Run single test
ctest --test-dir build -R scan_insert_test --output-on-failure

# Run Yosys with plugins
./build/yosys/bin/yosys -m ./build/passes/scan_insert.so
```

---

## Decision Log

| Date | Decision | Rationale |
|------|----------|-----------|
| 2026-02-15 | Use yosys-slang as submodule (not fork initially) | Start with upstream, fork only when DPI modifications needed |
| 2026-02-15 | Plugins don't link libyosys directly | Required for proper symbol resolution on macOS |
| 2026-02-15 | Use equiv_induct for sequential equivalence | Scan chain is sequential; need inductive proof |
| 2026-02-15 | Tie scan_enable=0 for equivalence check | Scan chain must be disabled to be functionally transparent |

