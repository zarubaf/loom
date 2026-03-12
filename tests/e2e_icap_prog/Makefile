# SPDX-License-Identifier: Apache-2.0
TOP     := icap_prog
DUT_SRC := icap_prog.sv

include ../../src/util/mk/loom_test.mk

# Generate the minimal test bitstream (sync word + 24 NOPs)
$(BUILD)/test.bit: gen_test_bit.py | $(BUILD)
	python3 $< $@

# Override test target: build sim + bitstream, then drive via loomx
test: $(_TEST_DEPS) $(BUILD)/test.bit
	$(LOOMX) -work $(BUILD) -sim Vloom_shell -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo "--- Checking output ---"
	@grep -q 'reconfigure:.*bytes'  $(BUILD)/test.log
	@grep -q 'reconfigure: done'    $(BUILD)/test.log
	@echo "PASS: ICAP programming e2e test passed"
