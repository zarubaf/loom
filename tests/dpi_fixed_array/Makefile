# SPDX-License-Identifier: Apache-2.0
# DPI fixed-size array test — verifies input/output fixed-size array handling
TOP      := dpi_fixed_array
DUT_SRC  := dpi_fixed_array.sv
DPI_SRCS := dpi_impl.c

include ../../src/util/mk/loom_test.mk

# Override test target to verify DPI call output
test: $(_TEST_DEPS)
	@echo "run" > $(BUILD)/test_script.txt
	@echo "exit" >> $(BUILD)/test_script.txt
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_shell \
		-f $(BUILD)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output ---"
	@grep -q 'dpi_sum_array("init", n=4)' $(BUILD)/test.log
	@grep -q 'data\[0\] = 0xdeadbeef' $(BUILD)/test.log
	@grep -q 'sum = 0x569daf15' $(BUILD)/test.log
	@grep -q 'dpi_fill_array("test", n=4)' $(BUILD)/test.log
	@grep -q 'dpi_sum_array("test", n=4)' $(BUILD)/test.log
	@grep -q 'sum = 0xaaaaaaaa' $(BUILD)/test.log
	@echo "PASS: all DPI fixed-size array checks passed"
