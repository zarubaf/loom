# SPDX-License-Identifier: Apache-2.0
# Loom — FPGA build Makefile (Alveo U250)

LOOM_ROOT    := $(shell cd .. && pwd)
LOOM_SRC     := $(LOOM_ROOT)/src
FPGA_DIR     := $(LOOM_ROOT)/fpga
IP_DIR       := $(FPGA_DIR)/ip
BOARD_DIR    := $(FPGA_DIR)/boards/u250
SCRIPTS_DIR  := $(FPGA_DIR)/scripts
WORK_DIR     := $(FPGA_DIR)/work-u250

VIVADO       ?= /opt/eda/xilinx/Vivado/2024.1/bin/vivado
VIVADO_FLAGS := -mode batch -nojournal -nolog

# Transformed DUT (user must supply or use tests/e2e to generate)
TRANSFORMED_V ?= $(LOOM_ROOT)/tests/e2e/build/transformed.v

# XDMA driver source
XDMA_DIR     := $(LOOM_ROOT)/third_party/dma_ip_drivers/XDMA/linux-kernel

# Board settings
export XILINX_PART       := xcu250-figd2104-2L-e
export XILINX_BOARD_PART := xilinx.com:au250:part0:1.3

# Export for TCL scripts
export LOOM_SRC
export BOARD_DIR
export IP_DIR
export WORK_DIR
export TRANSFORMED_V
export RM_NAME ?= rm

.PHONY: all ip synth bitstream program dfx-static dfx-rm dfx-program-full dfx-program-flash dfx-program-rm driver driver-load driver-unload rescan clean

all: bitstream

# ----------------------------------------------------------------
# IP Generation
# ----------------------------------------------------------------

ip: $(IP_DIR)/xlnx_xdma/.done $(IP_DIR)/xlnx_clk_gen/.done $(IP_DIR)/xlnx_cdc/.done $(IP_DIR)/xlnx_decoupler/.done

$(IP_DIR)/xlnx_xdma/.done:
	cd $(IP_DIR)/xlnx_xdma && $(VIVADO) $(VIVADO_FLAGS) -source tcl/run.tcl
	touch $@

$(IP_DIR)/xlnx_clk_gen/.done:
	cd $(IP_DIR)/xlnx_clk_gen && $(VIVADO) $(VIVADO_FLAGS) -source tcl/run.tcl
	touch $@

$(IP_DIR)/xlnx_cdc/.done:
	cd $(IP_DIR)/xlnx_cdc && $(VIVADO) $(VIVADO_FLAGS) -source tcl/run.tcl
	touch $@

$(IP_DIR)/xlnx_decoupler/.done:
	cd $(IP_DIR)/xlnx_decoupler && $(VIVADO) $(VIVADO_FLAGS) -source tcl/run.tcl
	touch $@

# ----------------------------------------------------------------
# Synthesis
# ----------------------------------------------------------------

IP_DONE := $(IP_DIR)/xlnx_xdma/.done $(IP_DIR)/xlnx_clk_gen/.done $(IP_DIR)/xlnx_cdc/.done $(IP_DIR)/xlnx_decoupler/.done

# Infrastructure RTL read by synth.tcl
LOOM_RTL := \
  $(LOOM_SRC)/rtl/loom_axi4_err_slv.sv \
  $(LOOM_SRC)/rtl/loom_axil_demux.sv \
  $(LOOM_SRC)/rtl/loom_emu_ctrl.sv \
  $(LOOM_SRC)/rtl/loom_dpi_regfile.sv \
  $(LOOM_SRC)/rtl/loom_scan_ctrl.sv \
  $(LOOM_SRC)/rtl/loom_axil_firewall.sv \
  $(LOOM_SRC)/rtl/loom_shell.sv

# Constraints and scripts
SYNTH_XDC := $(BOARD_DIR)/u250_pins.xdc $(BOARD_DIR)/u250_timing.xdc
SYNTH_TCL := $(SCRIPTS_DIR)/synth.tcl $(BOARD_DIR)/settings.tcl

synth: $(WORK_DIR)/results/synth.dcp

$(WORK_DIR)/results/synth.dcp: $(TRANSFORMED_V) $(LOOM_RTL) $(SYNTH_XDC) $(SYNTH_TCL) $(IP_DONE) | $(WORK_DIR)
	$(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/synth.tcl

$(WORK_DIR):
	mkdir -p $@/results

# ----------------------------------------------------------------
# Bitstream
# ----------------------------------------------------------------

bitstream: $(WORK_DIR)/results/loom_shell.bit

$(WORK_DIR)/results/loom_shell.bit: $(WORK_DIR)/results/synth.dcp
	$(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/impl.tcl

# ----------------------------------------------------------------
# DFX — Static build (run once to lock the shell)
# ----------------------------------------------------------------

DFX_STATIC_DONE := $(WORK_DIR)/results/static_routed.dcp

dfx-static: $(DFX_STATIC_DONE)

# Step 1: synthesize static (emu_top as black box)
$(WORK_DIR)/results/static_synth.dcp: $(LOOM_RTL) $(SYNTH_XDC) $(SYNTH_TCL) $(IP_DONE) | $(WORK_DIR)
	$(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/synth_static.tcl

# Step 2: synthesize RM OOC
$(WORK_DIR)/results/$(RM_NAME)_synth.dcp: $(TRANSFORMED_V) $(IP_DONE) | $(WORK_DIR)
	$(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/synth_rm.tcl

# Step 3: full DFX P&R — locks static, writes full.bit + rm_partial.bit
$(DFX_STATIC_DONE): $(WORK_DIR)/results/static_synth.dcp $(WORK_DIR)/results/$(RM_NAME)_synth.dcp
	$(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/dfx_impl.tcl

# ----------------------------------------------------------------
# DFX — Fast RM swap (reuses locked static_routed.dcp)
# ----------------------------------------------------------------
# static_routed.dcp must already exist (copy from the machine that ran
# dfx-static, or run dfx-static here first). dfx-rm never rebuilds it.

dfx-rm: $(WORK_DIR)/results/$(RM_NAME)_synth.dcp
	@test -f $(DFX_STATIC_DONE) || \
	  (echo "ERROR: $(DFX_STATIC_DONE) not found. Run 'make dfx-static' first, or copy static_routed.dcp here."; exit 1)
	$(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/dfx_rm.tcl

# ----------------------------------------------------------------
# Programming (all modes use scripts/program.tcl)
# ----------------------------------------------------------------

# Non-DFX full JTAG load (legacy flat bitstream)
program:
	MODE=jtag BIT_FILE=$(WORK_DIR)/results/loom_shell.bit \
	  $(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/program.tcl

# DFX: volatile full load for testing (flash unchanged, reverts on power cycle)
dfx-program-full:
	MODE=jtag BIT_FILE=$(WORK_DIR)/results/full.bit \
	  $(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/program.tcl

# DFX: write full.bit to SPI flash (persistent across power cycles)
dfx-program-flash:
	MODE=flash BIT_FILE=$(WORK_DIR)/results/full.bit \
	  $(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/program.tcl

# DFX: swap RP via JTAG partial bitstream (static shell survives)
dfx-program-rm:
	@test -n "$(PARTIAL_BIT)" || (echo "ERROR: set PARTIAL_BIT=path/to/partial.bit"; exit 1)
	MODE=jtag-partial BIT_FILE=$(PARTIAL_BIT) \
	  $(VIVADO) $(VIVADO_FLAGS) -source $(SCRIPTS_DIR)/program.tcl

rescan:
	sudo $(SCRIPTS_DIR)/pcie_rescan.sh

# ----------------------------------------------------------------
# XDMA Driver
# ----------------------------------------------------------------

driver:
	$(MAKE) -C $(XDMA_DIR)/xdma
	$(MAKE) -C $(XDMA_DIR)/tools

driver-load: driver
	sudo insmod $(XDMA_DIR)/xdma/xdma.ko
	@sleep 1
	@if [ -e /dev/xdma0_user ]; then \
		echo "OK: /dev/xdma0_user available"; \
	else \
		echo "WARNING: /dev/xdma0_user not found"; \
	fi

driver-unload:
	-sudo rmmod xdma

# ----------------------------------------------------------------
# Clean
# ----------------------------------------------------------------

clean:
	rm -rf $(WORK_DIR)
	rm -f $(IP_DIR)/xlnx_xdma/.done $(IP_DIR)/xlnx_clk_gen/.done $(IP_DIR)/xlnx_cdc/.done $(IP_DIR)/xlnx_decoupler/.done
