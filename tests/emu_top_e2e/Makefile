# SPDX-License-Identifier: Apache-2.0
# End-to-end emulation test Makefile

# Paths (adjust for your build location)
BUILD_DIR ?= ../../build
YOSYS_BIN ?= $(BUILD_DIR)/yosys/bin/yosys
SLANG_PLUGIN ?= $(BUILD_DIR)/yosys-slang/slang.so
SCAN_INSERT ?= $(BUILD_DIR)/passes/scan_insert/scan_insert.so
DPI_BRIDGE ?= $(BUILD_DIR)/passes/dpi_bridge/dpi_bridge.so
EMU_TOP ?= $(BUILD_DIR)/passes/emu_top/emu_top.so

RTL_DIR = ../../rtl

# Verilog simulator (iverilog by default)
IVERILOG ?= iverilog
VVP ?= vvp

.PHONY: all transform sim clean

all: sim

# Run Yosys to transform design
transformed.v: transform.ys ../fixtures/dpi_example.sv $(RTL_DIR)/loom_clk_gate.sv
	$(YOSYS_BIN) \
		-m $(SLANG_PLUGIN) \
		-m $(SCAN_INSERT) \
		-m $(DPI_BRIDGE) \
		-m $(EMU_TOP) \
		-s transform.ys

transform: transformed.v

# Compile simulation
# Note: loom_clk_gate.sv provides behavioral model (no SYNTHESIS define)
tb_emu_top.vvp: transformed.v tb_emu_top.sv $(RTL_DIR)/loom_host_stub.sv $(RTL_DIR)/loom_clk_gate.sv
	$(IVERILOG) -g2012 -o $@ \
		$(RTL_DIR)/loom_clk_gate.sv \
		$(RTL_DIR)/loom_host_stub.sv \
		transformed.v \
		tb_emu_top.sv

# Run simulation
sim: tb_emu_top.vvp
	$(VVP) $<

# View waveforms (if gtkwave is installed)
wave: sim
	gtkwave tb_emu_top.vcd &

clean:
	rm -f transformed.v tb_emu_top.vvp tb_emu_top.vcd
