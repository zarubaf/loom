# SPDX-License-Identifier: Apache-2.0
# Makefile for memory scan E2E test

# Paths
RTL_DIR = ../../rtl
FIXTURE_DIR = ../fixtures

# Source files
TB_SRC = tb_mem_scan.sv
RTL_SRC = $(RTL_DIR)/loom_mem_scan_ctrl.sv
FIXTURE_SRC = $(FIXTURE_DIR)/sram.sv

# Simulation binary
SIM_BIN = sim_mem_scan

# Default target
.PHONY: all
all: sim

# Compile with iverilog
$(SIM_BIN): $(TB_SRC) $(RTL_SRC) $(FIXTURE_SRC)
	iverilog -g2012 -o $@ $^

# Run simulation
.PHONY: sim
sim: $(SIM_BIN)
	./$(SIM_BIN)

# View waveform
.PHONY: wave
wave: tb_mem_scan.vcd
	gtkwave $< &

# Clean
.PHONY: clean
clean:
	rm -f $(SIM_BIN) *.vcd
