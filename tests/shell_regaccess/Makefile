# SPDX-License-Identifier: Apache-2.0
TOP      := shell_regaccess
DUT_SRC  := shell_regaccess.sv

include ../../src/util/mk/loom_test.mk

# Override test target to exercise shell read/write/status commands
test: $(_TEST_DEPS)
	$(LOOMX) -work $(BUILD) -sim Vloom_sim_top -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output ---"
	@grep -q 'Design ID:' $(BUILD)/test.log
	@grep -q '0x[0-9a-f]\{8\}' $(BUILD)/test.log
	@grep -q 'OK \[0x00010\]' $(BUILD)/test.log
	@echo "PASS: shell register access checks passed"
