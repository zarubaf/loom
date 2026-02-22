# SPDX-License-Identifier: Apache-2.0
# loom_sim.mk — Verilator simulation build rules (testing)
#
# Provides a pattern rule to build a Verilator binary from transformed
# Verilog + Loom infrastructure RTL.
#
# Requires: LOOM_HOME to be set before including.

LOOM_HOME ?= $(error LOOM_HOME is not set)

# Verilator binary (v5.044+, bug fixed)
# Prefer project-built Verilator over system one
_LOOM_VERILATOR_BUILD := $(LOOM_HOME)/build/verilator/bin/verilator
VERILATOR ?= $(if $(wildcard $(_LOOM_VERILATOR_BUILD)),$(_LOOM_VERILATOR_BUILD),verilator)

VERILATOR_FLAGS := \
    --binary --timing -Wall -Wno-fatal \
    -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-BLKSEQ -Wno-TIMESCALEMOD \
    --trace-fst --x-initial unique \
    -CFLAGS "-g -O0" -LDFLAGS "-lpthread"

# Infrastructure RTL (adapt for build tree vs install tree)
ifneq ($(wildcard $(LOOM_HOME)/src/rtl),)
  # Build tree
  _LOOM_RTL  := $(LOOM_HOME)/src/rtl
  _LOOM_BFM  := $(LOOM_HOME)/src/bfm
else
  # Install tree
  _LOOM_RTL  := $(LOOM_HOME)/share/loom/rtl
  _LOOM_BFM  := $(LOOM_HOME)/share/loom/bfm
endif

LOOM_SIM_RTL := \
    $(_LOOM_RTL)/loom_emu_ctrl.sv \
    $(_LOOM_RTL)/loom_axil_demux.sv \
    $(_LOOM_RTL)/loom_axil_firewall.sv \
    $(_LOOM_RTL)/loom_axi4_err_slv.sv \
    $(_LOOM_RTL)/loom_dpi_regfile.sv \
    $(_LOOM_RTL)/loom_scan_ctrl.sv \
    $(_LOOM_RTL)/loom_mem_ctrl.sv \
    $(_LOOM_RTL)/loom_shell.sv \
    $(_LOOM_BFM)/loom_axil_socket_bfm.sv \
    $(_LOOM_BFM)/xlnx_xdma.sv \
    $(_LOOM_BFM)/xlnx_clk_gen.sv \
    $(_LOOM_BFM)/xlnx_cdc.sv \
    $(_LOOM_BFM)/xlnx_decoupler.sv \
    $(_LOOM_BFM)/xilinx_primitives.sv

LOOM_SIM_DPI := $(_LOOM_BFM)/loom_sock_dpi.c

# Pattern rule: build Verilator sim from work dir containing transformed.v
# Usage: make <work>/sim/obj_dir/Vloom_shell
%/sim/obj_dir/Vloom_shell: %/transformed.v
	@mkdir -p $*/sim/obj_dir
	$(VERILATOR) $(VERILATOR_FLAGS) --top-module loom_shell \
	    $(LOOM_SIM_RTL) $< $(LOOM_SIM_DPI) \
	    --Mdir $*/sim/obj_dir -o Vloom_shell
