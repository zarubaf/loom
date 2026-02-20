# SPDX-License-Identifier: Apache-2.0
TOP      := scan_dump_test
DUT_SRC  := scan_dump_test.sv
DPI_SRCS := dpi_impl.c

include ../../src/util/mk/loom_test.mk

# Override test target to exercise scan dump with DPI calls
test: $(_TEST_DEPS)
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_shell -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output (per-snapshot) ---"
	@# snap_idle: after reset — StIdle, all DPI results zero
	@grep -A 20 'File:.*snap_idle' $(BUILD)/test.log | grep -q 'state_q.*StIdle (0x0)'
	@grep -A 20 'File:.*snap_idle' $(BUILD)/test.log | grep -q 'counter_q.*0xcafe'
	@grep -A 20 'File:.*snap_idle' $(BUILD)/test.log | grep -q 'add_result_q.*0x00000000'
	@grep -A 20 'File:.*snap_idle' $(BUILD)/test.log | grep -q 'step_count_q.*0x00'
	@# snap_call_add: StIdle -> StCallAdd (1 cycle)
	@grep -A 20 'File:.*snap_call_add' $(BUILD)/test.log | grep -q 'state_q.*StCallAdd (0x1)'
	@grep -A 20 'File:.*snap_call_add' $(BUILD)/test.log | grep -q 'step_count_q.*0x01'
	@# snap_call_notify: StCallAdd -> StCallNotify (dpi_add executed)
	@grep -A 20 'File:.*snap_call_notify' $(BUILD)/test.log | grep -q 'state_q.*StCallNotify (0x2)'
	@grep -A 20 'File:.*snap_call_notify' $(BUILD)/test.log | grep -q 'add_result_q.*0x0000dafe'
	@grep -A 20 'File:.*snap_call_notify' $(BUILD)/test.log | grep -q 'step_count_q.*0x02'
	@# snap_call_fill: StCallNotify -> StCallFill (dpi_notify executed)
	@grep -A 20 'File:.*snap_call_fill' $(BUILD)/test.log | grep -q 'state_q.*StCallFill (0x3)'
	@grep -A 20 'File:.*snap_call_fill' $(BUILD)/test.log | grep -q 'notify_done_q.*0x1'
	@grep -A 20 'File:.*snap_call_fill' $(BUILD)/test.log | grep -q 'step_count_q.*0x03'
	@# snap_call_sum: StCallFill -> StCallSum (dpi_fill_fixed executed)
	@grep -A 20 'File:.*snap_call_sum' $(BUILD)/test.log | grep -q 'state_q.*StCallSum (0x4)'
	@grep -A 20 'File:.*snap_call_sum' $(BUILD)/test.log | grep -q 'fill_ret_q.*0x00000004'
	@grep -A 20 'File:.*snap_call_sum' $(BUILD)/test.log | grep -q 'step_count_q.*0x04'
	@# snap_done: StCallSum -> StDone (dpi_sum_open executed)
	@grep -A 20 'File:.*snap_done' $(BUILD)/test.log | grep -q 'state_q.*StDone (0x5)'
	@grep -A 20 'File:.*snap_done' $(BUILD)/test.log | grep -q 'sum_result_q.*0xaaaaaaaa'
	@grep -A 20 'File:.*snap_done' $(BUILD)/test.log | grep -q 'step_count_q.*0x05'
	@# snap_count: 3 more cycles counting in StDone
	@grep -A 20 'File:.*snap_count' $(BUILD)/test.log | grep -q 'state_q.*StDone (0x5)'
	@grep -A 20 'File:.*snap_count' $(BUILD)/test.log | grep -q 'counter_q.*0xcb01'
	@grep -A 20 'File:.*snap_count' $(BUILD)/test.log | grep -q 'step_count_q.*0x08'
	@echo "PASS: scan dump variable checks passed"
