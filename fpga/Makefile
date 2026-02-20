# SPDX-License-Identifier: Apache-2.0
# Loom — FPGA build Makefile (Alveo U250)

LOOM_ROOT    := $(shell cd .. && pwd)
LOOM_SRC     := $(LOOM_ROOT)/src
FPGA_DIR     := $(LOOM_ROOT)/fpga
IP_DIR       := $(FPGA_DIR)/ip
BOARD_DIR    := $(FPGA_DIR)/boards/u250
SCRIPTS_DIR  := $(FPGA_DIR)/scripts
WORK_DIR     := $(FPGA_DIR)/work-u250

VIVADO       ?= /opt/eda/ubuntu-24.04/xilinx/Vivado/2024.1/bin/vivado
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

.PHONY: all ip synth bitstream program driver driver-load driver-unload rescan clean

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

synth: $(WORK_DIR)/results/synth.dcp

$(WORK_DIR)/results/synth.dcp: $(TRANSFORMED_V) $(IP_DONE) | $(WORK_DIR)
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
# Program + Rescan
# ----------------------------------------------------------------

program:
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
