# SPDX-License-Identifier: Apache-2.0
# Multisim read/write example — ported from Veloce to Loom
TOP      := top
DUT_SRC  := top.sv multisim_server_pull.sv multisim_server_pull_then_push.sv
DPI_SRCS := multisim/multisim_server.cpp multisim/socket_server/server.cpp
DPI_HDRS := multisim/multisim_server.h multisim/multisim_common.h \
            multisim/socket_server/server.h
DPI_CXXFLAGS := -Imultisim -DMULTISIM_EMULATION
LOOMC_FLAGS  := -D MULTISIM_EMULATION
timeout      := -1

include ../../src/util/mk/loom_test.mk

# Disable tracing for this test (large iteration counts make FST files huge)
VERILATOR_FLAGS := $(filter-out --trace-fst,$(VERILATOR_FLAGS))

# ---------- SW client ----------
SW_SRCS := sw_test.cpp multisim/multisim_client.cpp multisim/socket_server/client.cpp
SW_HDRS := multisim/multisim_client.h multisim/multisim_common.h \
           multisim/socket_server/client.h

$(BUILD)/sw_test: $(SW_SRCS) $(SW_HDRS) | $(BUILD)
	$(CXX) -std=c++17 -g -O0 -DMULTISIM_SW -Imultisim -o $@ $(filter %.cpp,$^)

# Override default test: run sim in background, then client, check results
test: $(_TEST_DEPS) $(BUILD)/sw_test
	@rm -rf .multisim && mkdir -p .multisim
	@echo "run" > $(BUILD)/test_script.txt
	@echo "exit" >> $(BUILD)/test_script.txt
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) $(_LOOMX_TIMEOUT) -sim Vloom_shell -f $(BUILD)/test_script.txt & \
	SIM_PID=$$!; \
	sleep 2; \
	$(BUILD)/sw_test; \
	SW_RC=$$?; \
	wait $$SIM_PID; \
	SIM_RC=$$?; \
	echo "--- sim exit: $$SIM_RC, client exit: $$SW_RC ---"; \
	[ $$SW_RC -eq 0 ] && [ $$SIM_RC -eq 0 ]

# ==========================================================================
# Reference: plain Verilator (no Loom), MULTISIM_SIMULATION mode
# ==========================================================================
REF_DIR := $(BUILD)/ref

REF_SV  := top.sv multisim_server_pull.sv multisim_server_pull_then_push.sv
REF_DPI := multisim/multisim_server.cpp multisim/socket_server/server.cpp

$(REF_DIR)/obj_dir/Vtop: $(REF_SV) $(REF_DPI) $(DPI_HDRS) | $(REF_DIR)
	$(VERILATOR) $(VERILATOR_FLAGS) --top-module top \
	    $(REF_SV) $(REF_DPI) \
	    -CFLAGS "-Imultisim -I$(LOOM_HOME)/src/include" \
	    --Mdir $(REF_DIR)/obj_dir -o Vtop

$(REF_DIR):
	@mkdir -p $@

.PHONY: ref ref-test ref-interactive

ref: $(REF_DIR)/obj_dir/Vtop $(BUILD)/sw_test

ref-test: $(REF_DIR)/obj_dir/Vtop $(BUILD)/sw_test
	@rm -rf .multisim && mkdir -p .multisim
	$(REF_DIR)/obj_dir/Vtop & \
	SIM_PID=$$!; \
	sleep 2; \
	$(BUILD)/sw_test; \
	SW_RC=$$?; \
	wait $$SIM_PID; \
	SIM_RC=$$?; \
	echo "--- sim exit: $$SIM_RC, client exit: $$SW_RC ---"; \
	[ $$SW_RC -eq 0 ] && [ $$SIM_RC -eq 0 ]
