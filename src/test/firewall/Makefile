# SPDX-License-Identifier: Apache-2.0
# Standalone Makefile for loom_axil_firewall testbench

LOOM_HOME ?= ../../..
BUILD     := build
VERILATOR ?= $(LOOM_HOME)/build/verilator/bin/verilator
CC        ?= cc

SOCKET    := $(BUILD)/fw_test.sock

VERILATOR_FLAGS := --binary --timing -Wall -Wno-fatal \
    -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-BLKSEQ -Wno-TIMESCALEMOD \
    --assert --trace-fst --trace-structs --trace-depth 99 --x-initial unique \
    -CFLAGS "-g -O0" -LDFLAGS "-lpthread"

SV_SRCS := tb_axil_firewall.sv fw_test_slave.sv \
    $(LOOM_HOME)/src/rtl/loom_axil_firewall.sv \
    $(LOOM_HOME)/src/rtl/loom_axil_demux.sv \
    $(LOOM_HOME)/src/bfm/loom_axil_socket_bfm.sv

DPI_SRC := $(LOOM_HOME)/src/bfm/loom_sock_dpi.c

SIM_BIN    := $(BUILD)/obj_dir/Vtb_axil_firewall
DRIVER_BIN := $(BUILD)/fw_test_driver

.PHONY: all test clean

all: $(SIM_BIN) $(DRIVER_BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(SIM_BIN): $(SV_SRCS) $(DPI_SRC) | $(BUILD)
	$(VERILATOR) $(VERILATOR_FLAGS) \
		--Mdir $(BUILD)/obj_dir \
		--top-module tb_axil_firewall \
		$(SV_SRCS) $(DPI_SRC)

$(DRIVER_BIN): fw_test_driver.c | $(BUILD)
	$(CC) -g -O0 -Wall -o $@ $<

test: $(SIM_BIN) $(DRIVER_BIN)
	@rm -f $(SOCKET)
	@echo "--- Starting simulation ---"
	$(SIM_BIN) +socket=$(SOCKET) +timeout=50000000 &
	@echo "--- Waiting for socket ---"
	@for i in $$(seq 1 50); do \
		if [ -e $(SOCKET) ]; then break; fi; \
		sleep 0.1; \
	done
	@if [ ! -e $(SOCKET) ]; then \
		echo "ERROR: Socket not created after 5s"; \
		exit 1; \
	fi
	$(DRIVER_BIN) $(SOCKET)
	@wait
	@echo "--- Test complete ---"

clean:
	rm -rf $(BUILD)
