# SPDX-License-Identifier: Apache-2.0
TOP      := assert_test
DUT_SRC  := assert_test.sv

include ../../src/util/mk/loom_test.mk

test: $(_TEST_DEPS)
	@echo "run" > $(BUILD)/test_script.txt
	@echo "exit" >> $(BUILD)/test_script.txt
	$(LOOMX) -work $(BUILD) -sim Vloom_shell \
		-f $(BUILD)/test_script.txt 2>&1 | tee $(BUILD)/test.log; true
	@grep -q 'Assertion failed' $(BUILD)/test.log || \
		{ echo "FAIL: no assertion failure detected"; exit 1; }
	@grep -q '\[assert_test\] started' $(BUILD)/test.log || \
		{ echo "FAIL: display output missing"; exit 1; }
	@grep -q '__loom_display_2' $(BUILD)/test.log || \
		{ echo "FAIL: else clause DPI function not registered"; exit 1; }
	@echo "PASS: assertions (immediate + concurrent + else clause) work"
