# SPDX-License-Identifier: Apache-2.0
# loom_sim.mk — Verilator simulation build rules (testing)
#
# Provides a pattern rule to build a Verilator binary from transformed
# Verilog + Loom infrastructure RTL.
#
# Requires: LOOM_HOME to be set before including.

LOOM_HOME ?= $(error LOOM_HOME is not set)

# Use debug binary to work around Verilator v5.040 internal fault bug
VERILATOR ?= VERILATOR_BIN=verilator_bin_dbg verilator

VERILATOR_FLAGS := \
    --binary --timing -Wall -Wno-fatal \
    -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-BLKSEQ -Wno-TIMESCALEMOD \
    --trace-fst -CFLAGS "-g -O0" -LDFLAGS "-lpthread"

# Infrastructure RTL (adapt for build tree vs install tree)
ifneq ($(wildcard $(LOOM_HOME)/src/rtl),)
  # Build tree
  _LOOM_RTL  := $(LOOM_HOME)/src/rtl
  _LOOM_BFM  := $(LOOM_HOME)/src/bfm
  _LOOM_TEST := $(LOOM_HOME)/src/test
else
  # Install tree
  _LOOM_RTL  := $(LOOM_HOME)/share/loom/rtl
  _LOOM_BFM  := $(LOOM_HOME)/share/loom/bfm
  _LOOM_TEST := $(LOOM_HOME)/share/loom/test
endif

LOOM_SIM_RTL := \
    $(_LOOM_RTL)/loom_emu_ctrl.sv \
    $(_LOOM_RTL)/loom_axil_demux.sv \
    $(_LOOM_RTL)/loom_dpi_regfile.sv \
    $(_LOOM_RTL)/loom_scan_ctrl.sv \
    $(_LOOM_BFM)/loom_axil_socket_bfm.sv \
    $(_LOOM_TEST)/loom_sim_top.sv

LOOM_SIM_DPI := $(_LOOM_BFM)/loom_sock_dpi.c

# Pattern rule: build Verilator sim from work dir containing transformed.v
# Usage: make <work>/sim/obj_dir/Vloom_sim_top
%/sim/obj_dir/Vloom_sim_top: %/transformed.v
	@mkdir -p $*/sim/obj_dir
	$(VERILATOR) $(VERILATOR_FLAGS) --top-module loom_sim_top \
	    $(LOOM_SIM_RTL) $< $(LOOM_SIM_DPI) \
	    --Mdir $*/sim/obj_dir -o Vloom_sim_top
