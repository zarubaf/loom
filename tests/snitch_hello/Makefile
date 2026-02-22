# SPDX-License-Identifier: Apache-2.0
# Snitch Hello World — Makefile for ref (Verilator) and Loom tests

LOOM_ROOT ?= $(realpath ../..)
export LOOM_HOME ?= $(LOOM_ROOT)

# RISC-V toolchain (auto-detect prefix: xPack uses riscv-none-elf-,
# others use riscv32-unknown-elf-)
_LOOM_TC := $(LOOM_ROOT)/third_party/riscv-toolchain/bin
RISCV_PREFIX ?= $(if $(wildcard $(_LOOM_TC)/riscv-none-elf-gcc),$(_LOOM_TC)/riscv-none-elf-,$(if $(wildcard $(_LOOM_TC)/riscv32-unknown-elf-gcc),$(_LOOM_TC)/riscv32-unknown-elf-,riscv-none-elf-))
RISCV_GCC    := $(RISCV_PREFIX)gcc
RISCV_OBJCOPY := $(RISCV_PREFIX)objcopy

# Verilator
_LOOM_VERILATOR_BUILD := $(LOOM_ROOT)/build/verilator/bin/verilator
VERILATOR ?= $(if $(wildcard $(_LOOM_VERILATOR_BUILD)),$(_LOOM_VERILATOR_BUILD),verilator)

# Loom tools
include $(LOOM_ROOT)/src/util/mk/loom.mk

BUILD := build

# Source filelist — single source of truth for both loomc and Verilator
SOURCES_F := sources.f

# Ref test Verilator flags (separate from loom_sim.mk's VERILATOR_FLAGS)
REF_VERILATOR_FLAGS := \
    --binary --timing --vpi -Wall -Wno-fatal \
    -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-BLKSEQ -Wno-TIMESCALEMOD \
    -Wno-CASEINCOMPLETE -Wno-UNSIGNED -Wno-CMPCONST -Wno-SHORTREAL \
    -DSYNTHESIS \
    --trace-fst --x-initial unique \
    -CFLAGS "-g -O0" -LDFLAGS "-lpthread"

.PHONY: all sw ref test clean

all: ref

# =========================================================================
# Software build
# =========================================================================
SW_DIR := sw
ELF    := $(BUILD)/hello.elf
BIN    := $(BUILD)/hello.bin
HEX    := $(BUILD)/program.hex

$(BUILD):
	@mkdir -p $@

$(ELF): $(SW_DIR)/crt0.S $(SW_DIR)/hello.c $(SW_DIR)/link.ld | $(BUILD)
	$(RISCV_GCC) -march=rv32i -mabi=ilp32 -O2 -nostdlib \
	    -T $(SW_DIR)/link.ld -o $@ $(SW_DIR)/crt0.S $(SW_DIR)/hello.c

$(BIN): $(ELF)
	$(RISCV_OBJCOPY) -O binary $< $@

$(HEX): $(BIN)
	xxd -e -g4 -c4 $< | awk '{print $$2}' > $@

sw: $(HEX)

# =========================================================================
# Reference test (plain Verilator, no Loom)
# =========================================================================
REF_OBJ := $(BUILD)/ref/obj_dir
REF_SIM := $(REF_OBJ)/Vtb_ref
REF_DPI := $(BUILD)/ref/dpi_impl.o

$(REF_DPI): dpi_impl.c | $(BUILD)
	@mkdir -p $(BUILD)/ref
	$(CC) -c -fPIC -g -O0 -I$(LOOM_ROOT)/src/include -o $@ $<

$(REF_SIM): $(SOURCES_F) tb_ref.sv $(REF_DPI) | $(BUILD)
	@mkdir -p $(REF_OBJ)
	$(VERILATOR) $(REF_VERILATOR_FLAGS) --top-module tb_ref \
	    -f $(SOURCES_F) tb_ref.sv \
	    $(CURDIR)/$(REF_DPI) \
	    --Mdir $(REF_OBJ) -o Vtb_ref

ref: $(REF_SIM) $(HEX)
	cp $(HEX) $(REF_OBJ)/program.hex
	cd $(REF_OBJ) && ./Vtb_ref 2>&1 | tee $(CURDIR)/$(BUILD)/ref.log
	@echo ""
	@echo "=== Checking ref output ==="
	@grep -q 'PASS: host swap OK' $(BUILD)/ref.log
	@echo "PASS: snitch hello ref test verified"

# =========================================================================
# Loom test (full pipeline)
# =========================================================================
TOP := snitch_hello_top

$(BUILD)/transformed.v: $(SOURCES_F) | $(BUILD)
	$(LOOMC) -top $(TOP) -work $(BUILD) -f $(SOURCES_F)

include $(LOOM_ROOT)/src/util/mk/loom_sim.mk

$(BUILD)/sim/obj_dir/Vloom_shell: $(BUILD)/transformed.v

$(BUILD)/libdpi.so: dpi_impl.c | $(BUILD)
	$(CC) -shared -fPIC -g -O0 -I$(LOOM_ROOT)/src/include -o $@ $< \
	    $(if $(filter Darwin,$(shell uname -s)),-undefined dynamic_lookup)

_TEST_DEPS := $(BUILD)/sim/obj_dir/Vloom_shell $(BUILD)/libdpi.so

test: $(_TEST_DEPS) $(HEX)
	cp $(HEX) program.hex
	$(LOOMX) -work $(BUILD) -sv_lib $(BUILD)/dpi -sim Vloom_shell \
	    -f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo ""
	@echo "=== Checking test output ==="
	@grep -q 'PASS: host swap OK' $(BUILD)/test.log
	@echo "PASS: snitch hello test verified"

# =========================================================================
# Clean
# =========================================================================
clean:
	rm -rf $(BUILD) program.hex
