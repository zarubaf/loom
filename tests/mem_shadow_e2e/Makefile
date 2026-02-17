# SPDX-License-Identifier: Apache-2.0
# Makefile for mem_shadow E2E test

YOSYS ?= ../../build/yosys/bin/yosys
YOSYS_SLANG_PLUGIN ?= ../../build/yosys-slang/slang.so
VERILATOR := verilator
OBJ_DIR := obj_dir

VERILATOR_FLAGS := \
    --binary --timing -Wall -Wno-fatal \
    -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-BLKSEQ -Wno-TIMESCALEMOD

.PHONY: all transform sim clean

all: sim

# Step 1: Transform the design with mem_shadow pass
transform: transformed_mem_test.v

transformed_mem_test.v: mem_test.sv test_mem_shadow.ys ../../build/passes/mem_shadow/mem_shadow.so
	$(YOSYS) -m $(YOSYS_SLANG_PLUGIN) -s test_mem_shadow.ys

# Step 2: Build and run Verilator simulation with testbench
sim: transformed_mem_test.v
	$(VERILATOR) $(VERILATOR_FLAGS) --top-module tb_mem_shadow \
	    --Mdir $(OBJ_DIR) -o Vsim_mem_shadow \
	    transformed_mem_test.v tb_mem_shadow.sv
	$(OBJ_DIR)/Vsim_mem_shadow

clean:
	rm -f transformed_mem_test.v mem_shadow_map.json
	rm -rf $(OBJ_DIR)
	rm -f *.vcd *.fst
