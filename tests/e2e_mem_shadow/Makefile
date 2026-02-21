# SPDX-License-Identifier: Apache-2.0
TOP      := mem_shadow_dut
DUT_SRC  := mem_shadow_dut.sv
DPI_SRCS := dpi_impl.c

include ../../src/util/mk/loom_test.mk

# Override test target for comprehensive memory shadow verification
test: $(_TEST_DEPS)
	@# Copy init files to build dir (loomx resolves relative to work dir)
	cp $(CURDIR)/rom_data.hex $(BUILD)/
	cp $(CURDIR)/lut_data.bin $(BUILD)/
	$(LOOMX) -work $(BUILD) $(_LOOMX_DPI) -sim Vloom_shell -timeout -1 \
		-f $(CURDIR)/test_script.txt 2>&1 | tee $(BUILD)/test.log
	@echo ""
	@echo "=== Checking test output ==="
	@echo ""
	@# 1. Verify status shows memories
	@echo "[check] status shows memories..."
	@grep -q 'Memories:.*[1-9]' $(BUILD)/test.log
	@echo "  OK"
	@echo ""
	@# 2. Verify DPI call executed
	@echo "[check] DPI call executed..."
	@grep -q 'dpi_checksum' $(BUILD)/test.log
	@echo "  OK"
	@echo ""
	@# 3. Verify initial state snapshot has scan variables
	@echo "[check] snap_init has scan variables..."
	@grep -A 40 'File:.*snap_init' $(BUILD)/test.log | grep -q 'counter_q'
	@echo "  OK"
	@echo ""
	@# 4. Verify after-DPI snapshot shows DPI result
	@echo "[check] snap_after_dpi shows StCompute..."
	@grep -A 40 'File:.*snap_after_dpi' $(BUILD)/test.log | grep -q 'state_q.*StCompute'
	@echo "  OK"
	@echo ""
	@# 5. Verify after-reset snapshot shows initial state restored
	@echo "[check] snap_after_reset shows StIdle..."
	@grep -A 40 'File:.*snap_after_reset' $(BUILD)/test.log | grep -q 'state_q.*StIdle'
	@echo "  OK"
	@echo ""
	@# 6. Verify memory preload message appears
	@echo "[check] memory preload happened..."
	@grep -q 'Preloading memory' $(BUILD)/test.log || grep -q 'Preloaded.*memor' $(BUILD)/test.log
	@echo "  OK"
	@echo ""
	@# 7. Verify memory re-preload on reset
	@echo "[check] memory re-preload on reset..."
	@# There should be at least 2 preload messages (initial + reset)
	@test $$(grep -c 'Preload' $(BUILD)/test.log) -ge 2
	@echo "  OK"
	@echo ""
	@# 8. Verify dump shows memory section
	@echo "[check] dump shows memories section..."
	@grep -q 'Memories:' $(BUILD)/test.log
	@echo "  OK"
	@echo ""
	@# 9. Verify initial RAM content in snap_init (before DUT runs)
	@echo "[check] RAM content correct after preload..."
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  1\] = 0x20'
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  7\] = 0x80'
	@echo "  OK"
	@echo ""
	@# 10. Verify submodule ROM content from $$readmemh (snap_init)
	@echo "[check] ROM content correct after preload ($$readmemh)..."
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  0\] = 0xde'
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  3\] = 0xef'
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  7\] = 0xbe'
	@echo "  OK"
	@echo ""
	@# 10b. Verify submodule LUT content from $$readmemb (snap_init)
	@echo "[check] LUT content correct after preload ($$readmemb)..."
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  0\] = 0x01'
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  1\] = 0x02'
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  2\] = 0x04'
	@grep -A 50 'File:.*snap_init' $(BUILD)/test.log | grep -q '\[  3\] = 0x08'
	@echo "  OK"
	@echo ""
	@# 11. Verify no initial blocks in transformed.v (all init via shadow preload)
	@echo "[check] no initial blocks in transformed.v..."
	@! grep -q 'initial' $(BUILD)/transformed.v
	@echo "  OK"
	@echo ""
	@# 12. Verify inspect shows memory content in snapshots
	@echo "[check] inspect shows memory content..."
	@grep -A 50 'File:.*snap_running' $(BUILD)/test.log | grep -q 'ram:.*16 x 8'
	@echo "  OK"
	@echo ""
	@# 13. Verify DPI call reads preloaded memory data
	@echo "[check] DPI call reads preloaded memory data..."
	@grep -q 'dpi_checksum(0x20, 0xaa) = 0x8a' $(BUILD)/test.log
	@echo "  OK"
	@echo ""
	@# 14. Verify reset restores ROM init values (after reset + step, ROM should be preloaded again)
	@echo "[check] ROM re-preloaded after reset..."
	@grep -A 50 'File:.*snap_post_reset_run' $(BUILD)/test.log | grep -q '\[  0\] = 0xde'
	@echo "  OK"
	@echo ""
	@echo "=== PASS: all mem_shadow E2E checks passed ==="
