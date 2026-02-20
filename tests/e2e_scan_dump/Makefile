# SPDX-License-Identifier: Apache-2.0
TOP      := scan_dump_test
DUT_SRC  := scan_dump_test.sv

include ../../src/util/mk/loom_test.mk

# Override test target to exercise scan dump with named variables
test: $(_TEST_DEPS)
	$(LOOMX) -work $(BUILD) -sim Vloom_shell -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output ---"
	@grep -q 'counter_q.*0xcafe' $(BUILD)/test.log
	@grep -q 'counter_q.*0xcb02' $(BUILD)/test.log
	@grep -q 'counter_q.*0xcb04' $(BUILD)/test.log
	@grep -q 'step_count_q.*0x04' $(BUILD)/test.log
	@echo "PASS: scan dump variable checks passed"
