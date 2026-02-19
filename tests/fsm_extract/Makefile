# SPDX-License-Identifier: Apache-2.0
# FSM extraction equivalence tests — auto-discovery harness
#
# Drop a <name>_fsm.sv file into this directory and `make sim` will:
#   1. Transform it via gen_equiv.tcl (Yosys + slang)
#   2. Build an equivalence testbench with Verilator --timing
#   3. Run DUT vs. behavioural reference for 1000 random cycles

YOSYS     ?= ../../build/yosys/bin/yosys
SLANG_SO  ?= ../../build/yosys-slang/slang.so
VERILATOR ?= verilator

BUILD_DIR := obj_dir

# Auto-discover all *_fsm.sv source files
FSM_SRCS  := $(wildcard *_fsm.sv)
FSM_NAMES := $(FSM_SRCS:.sv=)
SIM_BINS  := $(addprefix $(BUILD_DIR)/Vtb_,$(addsuffix _equiv,$(FSM_NAMES)))

.PHONY: all sim clean

all: sim

# --- Transform + generate testbench (grouped target, Make >= 4.3) ---
transformed_%_fsm.v tb_%_fsm_equiv.sv &: %_fsm.sv gen_equiv.tcl $(SLANG_SO)
	$(YOSYS) -m $(SLANG_SO) -p "tcl gen_equiv.tcl $*_fsm"

# --- Build Verilator binary ---
$(BUILD_DIR)/Vtb_%_fsm_equiv: transformed_%_fsm.v %_fsm.sv tb_%_fsm_equiv.sv
	$(VERILATOR) --binary --timing --debug \
		-Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-CASEINCOMPLETE \
		--top-module tb_$*_fsm_equiv \
		transformed_$*_fsm.v $*_fsm.sv tb_$*_fsm_equiv.sv

# --- Run all equivalence simulations ---
sim: $(SIM_BINS)
	@for bin in $^; do $$bin || exit 1; done

clean:
	rm -rf $(BUILD_DIR) transformed_*_fsm.v tb_*_fsm_equiv.sv _ports.json
