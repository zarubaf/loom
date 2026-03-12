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

# ----------------------------------------------------------------
# Per-run structured logging and artifact management
# Usage: $(call vivado_run,<step>,<tcl>[,<extra KEY=VALUE env>])
#
# Each invocation creates:
#   $(WORK_DIR)/runs/<timestamp>_<step>_<rm>/      (default)
#   or $RUN_DIR/                                   (if RUN_DIR is set)
#     vivado.log  vivado.jou  metadata.txt  *.bit  *.rpt
#
# *.bit and *.rpt are moved into the run dir after completion;
# results/ holds symlinks to the latest run that produced them.
# *.dcp files stay as real files in results/ (inputs to downstream steps).
#
# After the run, results/ symlinks are force-updated to the new run dir.
# A failed run leaves the previous symlinks intact.
# ----------------------------------------------------------------
define vivado_run
{ \
  _d="$${RUN_DIR:-$(WORK_DIR)/runs/$$(date +%Y%m%d_%H%M%S)_$(1)_$(RM_NAME)}"; \
  mkdir -p "$$_d" "$(WORK_DIR)/results"; \
  printf 'step=$(1)\nrm_name=$(RM_NAME)\ntimestamp=%s\n' "$$(date -Iseconds)" \
    > "$$_d/metadata.txt"; \
  env $(3) LOOM_RUN_DIR="$$_d" $(VIVADO) -mode batch \
    -log     "$$_d/vivado.log" \
    -journal "$$_d/vivado.jou" \
    -source  "$(2)"; \
  _e=$$?; \
  printf 'exit_code=%d\n' "$$_e" >> "$$_d/metadata.txt"; \
  for _f in "$$_d/results"/*.bit "$$_d/results"/*.dcp "$$_d/results"/*.hash; do \
    [ -e "$$_f" ] || continue; \
    ln -sf "$$_f" "$(WORK_DIR)/results/$$(basename $$_f)"; \
  done; \
  exit $$_e; \
}
endef

# Transformed DUT (user must supply or use tests/e2e to generate)
TRANSFORMED_V ?= $(LOOM_ROOT)/tests/e2e/build/transformed.v

# XDMA driver — install via: cd dkms/xdma-dkms && make deb && sudo apt install ./xdma-dkms_*.deb

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
# Override to pin all vivado_run invocations to a specific directory instead of
# the default auto-generated timestamp path ($(WORK_DIR)/runs/<ts>_<step>_<rm>).
export RUN_DIR ?=

.PHONY: all ip synth bitstream program dfx-static dfx-rm dfx-program-full dfx-program-flash dfx-program-rm driver driver-load driver-unload rescan clean

all: bitstream

# ----------------------------------------------------------------
# IP Generation (one-time; logs suppressed — not worth keeping)
# ----------------------------------------------------------------

ip: $(IP_DIR)/xlnx_xdma/.done $(IP_DIR)/xlnx_clk_gen/.done $(IP_DIR)/xlnx_cdc/.done $(IP_DIR)/xlnx_decoupler/.done

$(IP_DIR)/xlnx_xdma/.done:
	cd $(IP_DIR)/xlnx_xdma && $(VIVADO) -mode batch -nojournal -nolog -source tcl/run.tcl
	touch $@

$(IP_DIR)/xlnx_clk_gen/.done:
	cd $(IP_DIR)/xlnx_clk_gen && $(VIVADO) -mode batch -nojournal -nolog -source tcl/run.tcl
	touch $@

$(IP_DIR)/xlnx_cdc/.done:
	cd $(IP_DIR)/xlnx_cdc && $(VIVADO) -mode batch -nojournal -nolog -source tcl/run.tcl
	touch $@

$(IP_DIR)/xlnx_decoupler/.done:
	cd $(IP_DIR)/xlnx_decoupler && $(VIVADO) -mode batch -nojournal -nolog -source tcl/run.tcl
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
  $(LOOM_SRC)/rtl/loom_icap_ctrl.sv \
  $(LOOM_SRC)/rtl/loom_shell.sv

# Constraints and scripts
SYNTH_XDC := $(BOARD_DIR)/u250_pins.xdc $(BOARD_DIR)/u250_timing.xdc
SYNTH_TCL := $(SCRIPTS_DIR)/synth.tcl $(BOARD_DIR)/settings.tcl

synth: $(WORK_DIR)/results/synth.dcp

$(WORK_DIR)/results/synth.dcp: $(TRANSFORMED_V) $(LOOM_RTL) $(SYNTH_XDC) $(SYNTH_TCL) $(IP_DONE) | $(WORK_DIR)
	@$(call vivado_run,synth,$(SCRIPTS_DIR)/synth.tcl)

$(WORK_DIR):
	mkdir -p $@/results $@/runs

# ----------------------------------------------------------------
# Bitstream
# ----------------------------------------------------------------

bitstream: $(WORK_DIR)/results/loom_shell.bit

$(WORK_DIR)/results/loom_shell.bit: $(WORK_DIR)/results/synth.dcp
	@$(call vivado_run,impl,$(SCRIPTS_DIR)/impl.tcl)

# ----------------------------------------------------------------
# DFX — Static build (run once to lock the shell)
# ----------------------------------------------------------------

DFX_STATIC_DONE    := $(WORK_DIR)/results/static_routed.dcp
DFX_FULL_BIT       := $(WORK_DIR)/results/full.bit
DFX_RM_PARTIAL_BIT := $(WORK_DIR)/results/$(RM_NAME)_partial.bit

dfx-static: $(DFX_STATIC_DONE) $(DFX_FULL_BIT) $(DFX_RM_PARTIAL_BIT)

# Step 1: synthesize static (emu_top as black box)
$(WORK_DIR)/results/static_synth.dcp: $(LOOM_RTL) $(SYNTH_XDC) $(SYNTH_TCL) $(IP_DONE) | $(WORK_DIR)
	@$(call vivado_run,synth_static,$(SCRIPTS_DIR)/synth_static.tcl)

# Step 2: synthesize RM OOC
$(WORK_DIR)/results/$(RM_NAME)_synth.dcp: $(TRANSFORMED_V) $(IP_DONE) | $(WORK_DIR)
	@$(call vivado_run,synth_rm,$(SCRIPTS_DIR)/synth_rm.tcl)

# Step 3: full DFX P&R — locks static, writes full.bit + rm_partial.bit
# Grouped target (&:) — one recipe produces all three outputs; Make re-runs if any is missing.
$(DFX_STATIC_DONE) $(DFX_FULL_BIT) $(DFX_RM_PARTIAL_BIT) &: \
    $(WORK_DIR)/results/static_synth.dcp $(WORK_DIR)/results/$(RM_NAME)_synth.dcp
	@$(call vivado_run,dfx_impl,$(SCRIPTS_DIR)/dfx_impl.tcl)

# ----------------------------------------------------------------
# DFX — Fast RM swap (reuses locked static_routed.dcp)
# ----------------------------------------------------------------
# static_routed.dcp must already exist (copy from the machine that ran
# dfx-static, or run dfx-static here first). dfx-rm never rebuilds it.

dfx-rm: $(WORK_DIR)/results/$(RM_NAME)_synth.dcp
	@test -f $(DFX_STATIC_DONE) || \
	  (echo "ERROR: $(DFX_STATIC_DONE) not found. Run 'make dfx-static' first, or copy static_routed.dcp here."; exit 1)
	@$(call vivado_run,dfx_rm,$(SCRIPTS_DIR)/dfx_rm.tcl)

# ----------------------------------------------------------------
# Programming (all modes use scripts/program.tcl)
# ----------------------------------------------------------------

# Non-DFX full JTAG load (legacy flat bitstream)
program:
	@$(call vivado_run,program,$(SCRIPTS_DIR)/program.tcl,MODE=jtag BIT_FILE=$(WORK_DIR)/results/loom_shell.bit)

# DFX: volatile full load for testing (flash unchanged, reverts on power cycle)
dfx-program-full:
	@$(call vivado_run,program,$(SCRIPTS_DIR)/program.tcl,MODE=jtag BIT_FILE=$(WORK_DIR)/results/full.bit)

# DFX: write full.bit to SPI flash (persistent across power cycles)
dfx-program-flash:
	@$(call vivado_run,program,$(SCRIPTS_DIR)/program.tcl,MODE=flash BIT_FILE=$(WORK_DIR)/results/full.bit)

# DFX: swap RP via JTAG partial bitstream (static shell survives)
dfx-program-rm:
	@$(call vivado_run,program,$(SCRIPTS_DIR)/program.tcl,MODE=jtag-partial BIT_FILE=$(WORK_DIR)/results/$(RM_NAME)_partial.bit)

rescan:
	sudo $(SCRIPTS_DIR)/pcie_rescan.sh

# ----------------------------------------------------------------
# XDMA Driver
# ----------------------------------------------------------------

driver:
	@echo "Build the DKMS .deb:  cd $(LOOM_ROOT)/dkms/xdma-dkms && make deb"
	@echo "Install:              sudo apt install $(LOOM_ROOT)/dkms/xdma-dkms/../xdma-dkms_*.deb"

driver-load:
	sudo modprobe xdma
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
