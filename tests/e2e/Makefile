# SPDX-License-Identifier: Apache-2.0
# Makefile for dpi_test e2e test

LOOM_ROOT := $(shell cd ../.. && pwd)
LOOM_SRC := $(LOOM_ROOT)/src
BFM_DIR := $(LOOM_SRC)/bfm
HOST_DIR := $(LOOM_SRC)/host
RTL_DIR := $(LOOM_SRC)/rtl
TEST_DIR := $(LOOM_SRC)/test
BUILD_DIR := build

SOCKET_PATH := /tmp/loom_sim.sock
OBJ_DIR := $(BUILD_DIR)/obj_dir

# Yosys with plugins (absolute paths)
YOSYS := $(LOOM_ROOT)/build/yosys/bin/yosys
YOSYS_PLUGINS := \
    -m $(LOOM_ROOT)/build/yosys-slang/slang.so \
    -m $(LOOM_ROOT)/build/passes/dpi_bridge/dpi_bridge.so \
    -m $(LOOM_ROOT)/build/passes/scan_insert/scan_insert.so \
    -m $(LOOM_ROOT)/build/passes/emu_top/emu_top.so

# Use debug binary to work around Verilator v5.040 internal fault bug
VERILATOR := VERILATOR_BIN=verilator_bin_dbg verilator
CC := gcc
CXX := g++

VERILATOR_FLAGS := \
    --binary --timing -Wall -Wno-fatal \
    -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM \
    -Wno-WIDTHTRUNC -Wno-WIDTHEXPAND -Wno-BLKSEQ -Wno-TIMESCALEMOD \
    --trace-fst -CFLAGS "-g -O0" -LDFLAGS "-lpthread"

CFLAGS := -g -O0 -Wall -I$(HOST_DIR)
CXXFLAGS := -std=c++20 -g -O0 -Wall -I$(HOST_DIR)
LDFLAGS := -lpthread

# RTL sources (absolute paths)
RTL_SRCS := \
    $(RTL_DIR)/loom_emu_ctrl.sv \
    $(RTL_DIR)/loom_axi_interconnect.sv \
    $(RTL_DIR)/loom_dpi_regfile.sv \
    $(RTL_DIR)/loom_emu_wrapper.sv \
    $(RTL_DIR)/loom_clk_gate.sv \
    $(RTL_DIR)/loom_scan_ctrl.sv \
    $(BFM_DIR)/loom_axil_socket_bfm.sv \
    $(BUILD_DIR)/transformed_dpi_test.v \
    $(TEST_DIR)/loom_sim_top.sv

DPI_SRCS := $(BFM_DIR)/loom_sock_dpi.c

# Host sources (from loom src/host) - Modern C++
HOST_LIB_SRCS := \
    $(HOST_DIR)/loom.cpp \
    $(HOST_DIR)/loom_transport_socket.cpp \
    $(HOST_DIR)/loom_dpi_service.cpp \
    $(HOST_DIR)/loom_vpi.cpp \
    $(HOST_DIR)/loom_sim_main.cpp

# User DPI implementations + generated dispatch table
HOST_SRCS := dpi_impl.c $(BUILD_DIR)/loom_dpi_dispatch.c

.PHONY: all clean transform sim host run

all: run

# Step 1: Transform SV to Verilog using Yosys
transform: $(BUILD_DIR)/transformed_dpi_test.v $(BUILD_DIR)/loom_dpi_dispatch.c

$(BUILD_DIR)/run.ys: | $(BUILD_DIR)
	@echo "read_slang ../dpi_test.sv -top dpi_test" > $@
	@echo "proc" >> $@
	@echo "dpi_bridge -json_out dpi_meta.json -header_out loom_dpi_dispatch.c" >> $@
	@echo "scan_insert -map scan_map.json" >> $@
	@echo "emu_top -top dpi_test -n_dpi_funcs 2 -scan_chain_length 64" >> $@
	@echo "write_verilog -noattr transformed_dpi_test.v" >> $@

$(BUILD_DIR)/transformed_dpi_test.v $(BUILD_DIR)/loom_dpi_dispatch.c: dpi_test.sv $(BUILD_DIR)/run.ys
	cd $(BUILD_DIR) && $(YOSYS) $(YOSYS_PLUGINS) -s run.ys

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Step 2: Build Verilator simulation
sim: $(OBJ_DIR)/Vloom_sim_top

$(OBJ_DIR)/Vloom_sim_top: $(RTL_SRCS) $(DPI_SRCS) transform
	@mkdir -p $(OBJ_DIR)
	$(VERILATOR) $(VERILATOR_FLAGS) --top-module loom_sim_top $(RTL_SRCS) $(DPI_SRCS) --Mdir $(OBJ_DIR) -o Vloom_sim_top

# Step 3: Build host binary
# Compile C files as C (to preserve extern linkage for const globals)
# Compile C++ files as C++, then link everything together
host: $(OBJ_DIR)/loom_host

# C object files (local sources)
HOST_C_OBJS := $(OBJ_DIR)/dpi_impl.o $(OBJ_DIR)/loom_dpi_dispatch.o

# C++ object files (from HOST_DIR)
HOST_CXX_OBJS := $(patsubst $(HOST_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(HOST_LIB_SRCS))

# Pattern rule for C++ files from HOST_DIR
$(OBJ_DIR)/%.o: $(HOST_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# Local C files need explicit rules
$(OBJ_DIR)/dpi_impl.o: dpi_impl.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR)/loom_dpi_dispatch.o: $(BUILD_DIR)/loom_dpi_dispatch.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	@mkdir -p $@

$(OBJ_DIR)/loom_host: $(HOST_C_OBJS) $(HOST_CXX_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Step 4: Run the test
run: sim host
	@rm -f $(SOCKET_PATH)
	@echo "=== Starting Loom Simulation ==="
	@$(OBJ_DIR)/Vloom_sim_top &
	@SIM_PID=$$!; \
	sleep 1; \
	echo "=== Running Host ===" ; \
	$(OBJ_DIR)/loom_host $(SOCKET_PATH); \
	TEST_RC=$$?; \
	kill $$SIM_PID 2>/dev/null || true; \
	wait $$SIM_PID 2>/dev/null || true; \
	rm -f $(SOCKET_PATH); \
	exit $$TEST_RC

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(SOCKET_PATH) *.vcd *.fst
