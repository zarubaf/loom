#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Full e2e test using actual DPI transformation pipeline
#
# Flow:
# 1. yosys-slang reads DPI imports -> creates $__loom_dpi_call cells
# 2. loom_instrument transforms DPI cells -> loom_dpi_* interface + flop enable
# 3. emu_top creates emulation wrapper with loom_en
# 4. loom_gen_sim.py generates simulation infrastructure
# 5. Verilator builds and runs simulation + host

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOOM_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Plugin paths
YOSYS_BIN="${LOOM_ROOT}/build/yosys/bin/yosys"
SLANG_PLUGIN="${LOOM_ROOT}/build/yosys-slang/slang.so"
LOOM_INSTRUMENT_PLUGIN="${LOOM_ROOT}/build/passes/loom_instrument/loom_instrument.so"
EMU_TOP_PLUGIN="${LOOM_ROOT}/build/passes/emu_top/emu_top.so"

echo "=== Loom Full E2E DPI Test ==="
echo "LOOM_ROOT: $LOOM_ROOT"
echo "BUILD_DIR: $BUILD_DIR"
echo ""
echo "Flow: DPI SV -> yosys-slang -> loom_instrument -> emu_top -> simulation"

# Check prerequisites
if [ ! -f "$YOSYS_BIN" ]; then
    echo "ERROR: Yosys not found at $YOSYS_BIN"
    echo "Please build loom first: cmake --build build"
    exit 1
fi

if [ ! -f "$SLANG_PLUGIN" ]; then
    echo "ERROR: slang plugin not found at $SLANG_PLUGIN"
    exit 1
fi

if [ ! -f "$LOOM_INSTRUMENT_PLUGIN" ]; then
    echo "ERROR: loom_instrument plugin not found at $LOOM_INSTRUMENT_PLUGIN"
    exit 1
fi

# Clean previous build
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

# Step 1: Transform DPI -> hardware interface
echo ""
echo "=== Step 1: Yosys transformation ==="
echo "  - Read DPI imports with yosys-slang"
echo "  - Transform DPI calls with loom_instrument"
echo "  - Generate emu_top wrapper with loom_en"

"$YOSYS_BIN" -p "
    plugin -i $SLANG_PLUGIN
    plugin -i $LOOM_INSTRUMENT_PLUGIN
    plugin -i $EMU_TOP_PLUGIN

    # Read the DUT with DPI imports
    read_slang $SCRIPT_DIR/dpi_test.sv

    # Elaborate
    hierarchy -check -top dpi_test

    # Process always blocks
    proc
    opt

    # Transform DPI calls to hardware interface
    loom_instrument -json_out $BUILD_DIR/dpi_metadata.json

    # Add emu_top wrapper
    emu_top -top dpi_test -clk clk_i -rst rst_ni

    # Cleanup
    opt_clean

    # Write output
    write_verilog -noattr $BUILD_DIR/transformed.v
" 2>&1 | tee "$BUILD_DIR/yosys.log"

echo "Transformation complete. Output: $BUILD_DIR/transformed.v"

# Step 2: Generate simulation infrastructure
echo ""
echo "=== Step 2: Generate simulation infrastructure ==="
python3 "$LOOM_ROOT/scripts/loom_gen_sim.py" \
    --dpi-json "$BUILD_DIR/dpi_metadata.json" \
    --module dpi_test \
    --dut-sv "$BUILD_DIR/transformed.v" \
    --loom-src "$LOOM_ROOT/src" \
    --output-dir "$BUILD_DIR"

# Step 3: Copy DPI implementation
echo ""
echo "=== Step 3: Copy DPI implementation ==="
cp "$SCRIPT_DIR/dpi_impl.c" "$BUILD_DIR/"

# Step 4: Build simulation
echo ""
echo "=== Step 4: Build Verilator simulation ==="
cd "$BUILD_DIR"
make sim

# Step 5: Build host
echo ""
echo "=== Step 5: Build host program ==="
make host

# Step 6: Run test
echo ""
echo "=== Step 6: Run simulation + host ==="
make run

echo ""
echo "=== Full E2E Test Complete ==="
