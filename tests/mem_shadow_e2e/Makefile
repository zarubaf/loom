# SPDX-License-Identifier: Apache-2.0
# Makefile for mem_shadow E2E test

YOSYS ?= ../../build/yosys/bin/yosys
IVERILOG ?= iverilog
VVP ?= vvp

.PHONY: all transform sim clean

all: sim

# Step 1: Transform the design with mem_shadow pass
transform: transformed_mem_test.v

transformed_mem_test.v: mem_test.sv test_mem_shadow.ys ../../build/passes/mem_shadow/mem_shadow.so
	$(YOSYS) -s test_mem_shadow.ys

# Step 2: Simulate with testbench
sim: transform
	$(IVERILOG) -g2012 -o sim_mem_shadow transformed_mem_test.v tb_mem_shadow.sv
	$(VVP) sim_mem_shadow

clean:
	rm -f transformed_mem_test.v mem_shadow_map.json sim_mem_shadow *.vcd
