# SPDX-License-Identifier: Apache-2.0
TOP      := scan_dump_test
DUT_SRC  := scan_dump_test.sv
DPI_SRCS := dpi_impl.c

include ../../src/util/mk/loom_test.mk

# Override test target to exercise scan dump with DPI calls
test: $(_TEST_DEPS)
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_shell -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output ---"
	@# Reset state
	@grep -q 'counter_q.*0xcafe' $(BUILD)/test.log
	@grep -q 'add_result_q.*0x00000000' $(BUILD)/test.log
	@# After DPI calls (snap_dpi)
	@grep -q 'add_result_q.*0x0000dafe' $(BUILD)/test.log
	@grep -q 'notify_done_q.*0x1' $(BUILD)/test.log
	@grep -q 'fill_ret_q.*0x00000004' $(BUILD)/test.log
	@grep -q 'sum_result_q.*0xaaaaaaaa' $(BUILD)/test.log
	@# After counting (snap_count)
	@grep -q 'counter_q.*0xcb01' $(BUILD)/test.log
	@grep -q 'step_count_q.*0x08' $(BUILD)/test.log
	@echo "PASS: scan dump variable checks passed"
