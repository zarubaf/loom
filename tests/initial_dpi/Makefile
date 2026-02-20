# SPDX-License-Identifier: Apache-2.0
TOP      := initial_dpi_test
DUT_SRC  := initial_dpi_test.sv
DPI_SRCS := dpi_impl.c

include ../../src/util/mk/loom_test.mk

# Override test target to verify initial DPI calls
test: $(_TEST_DEPS)
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_shell -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output ---"
	@# Verify init_setup was called
	@grep -q 'init_setup called with tag: test_tag' $(BUILD)/test.log
	@# Verify get_init_val was called with seed=42
	@grep -q 'get_init_val(42)' $(BUILD)/test.log
	@# After 1 step: init_reg_q should be 0x2FC89 (0x2FC88 + 1)
	@grep -A 20 'File:.*snap_step1' $(BUILD)/test.log | grep -q 'init_reg_q.*0x0002fc89'
	@# Counter should be incrementing
	@grep -A 20 'File:.*snap_step1' $(BUILD)/test.log | grep -q 'counter_q.*0x01'
	@echo "PASS: initial DPI test passed"
