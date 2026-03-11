# SPDX-License-Identifier: Apache-2.0
TOP      := dpi_fifo_test
DUT_SRC  := dpi_fifo_test.sv
DPI_SRCS := dpi_impl.c

include ../../src/util/mk/loom_test.mk

test: $(_TEST_DEPS)
	@echo "run" > $(BUILD)/test_script.txt
	@echo "exit" >> $(BUILD)/test_script.txt
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_shell \
		-f $(BUILD)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@grep -q 'TEST PASSED' $(BUILD)/test.log || \
		{ echo "FAIL: test did not pass"; exit 1; }
	@grep -q 'log_count=' $(BUILD)/test.log || \
		{ echo "FAIL: no log_count in output"; exit 1; }
	@echo "PASS: DPI FIFO test"
