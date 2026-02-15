# Loom

Open-source FPGA emulation toolchain. Transforms simulation-grade SystemVerilog (including DPI-C calls) into FPGA-synthesizable RTL with scan chain instrumentation.

## Building

```bash
# Prerequisites (macOS)
brew install pkg-config libffi bison readline

# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Test
ctest --test-dir build

# Install (optional)
cmake --install build --prefix ~/.local
```

## Usage

```bash
# Transform SystemVerilog for FPGA emulation
loom -f design.f -o output.v

# With top module
loom -f design.f -o output.v -top my_soc

# Include DPI-to-hardware bridge
loom -f design.f -o output.v --dpi-bridge
```

## What It Does

1. **Scan chain insertion** - Adds `loom_scan_*` ports for state capture/restore
2. **DPI bridge generation** - Converts DPI-C function calls to hardware mailbox interfaces (`loom_dpi_*` ports)

## License

Apache-2.0
