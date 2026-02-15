# Makefile for scan controller E2E test
# SPDX-License-Identifier: Apache-2.0

LOOM_ROOT := ../..
BUILD_DIR := $(LOOM_ROOT)/build
YOSYS := $(BUILD_DIR)/yosys/bin/yosys
SLANG_PLUGIN := $(BUILD_DIR)/yosys-slang/slang.so
SCAN_INSERT := $(BUILD_DIR)/passes/scan_insert/scan_insert.so
EMU_TOP := $(BUILD_DIR)/passes/emu_top/emu_top.so
RTL_DIR := $(LOOM_ROOT)/rtl

# RTL files needed for simulation
RTL_FILES := $(RTL_DIR)/loom_clk_gate.sv \
             $(RTL_DIR)/loom_scan_ctrl.sv

.PHONY: all transform sim clean wave

all: sim

# Transform counter design with scan chain insertion
transform: transformed_counter.v

transformed_counter.v: counter.sv transform_counter.ys $(SLANG_PLUGIN) $(SCAN_INSERT) $(EMU_TOP)
	$(YOSYS) -m $(SLANG_PLUGIN) \
	         -m $(SCAN_INSERT) \
	         -m $(EMU_TOP) \
	         -s transform_counter.ys 2>&1 | tee yosys.log

# Run simulation
sim: tb_scan_counter.vvp
	vvp $<

# Compile testbench
tb_scan_counter.vvp: transformed_counter.v tb_scan_counter.sv $(RTL_FILES)
	iverilog -g2012 -o $@ \
		$(RTL_FILES) \
		transformed_counter.v \
		tb_scan_counter.sv

# View waveforms
wave: tb_scan_counter.vcd
	gtkwave $< &

tb_scan_counter.vcd: sim

# Clean all generated files
clean:
	rm -f transformed_counter.v scan_map_counter.json \
	      tb_scan_counter.vvp tb_scan_counter.vcd \
	      yosys.log
