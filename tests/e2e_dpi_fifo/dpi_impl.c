// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for DPI FIFO test
//
// dpi_log:           read-only (void, all inputs) → FIFO path
// dpi_add:           read-write (returns int) → regfile path
// dpi_report_result: read-write (returns int) → regfile path

#include <stdio.h>
#include <stdint.h>
#include <assert.h>

static int log_count = 0;
static int last_log_cycle = -1;

// Read-only DPI function — should be routed through FIFO
void dpi_log(int32_t cycle, int32_t value) {
    // Verify monotonically increasing cycle (ordering guarantee)
    if (last_log_cycle >= 0) {
        assert(cycle > last_log_cycle);
    }
    last_log_cycle = cycle;
    log_count++;
    if (log_count <= 5 || log_count % 100 == 0) {
        printf("[dpi_log] cycle=%d value=0x%x (count=%d)\n", cycle, value, log_count);
    }
}

// Read-write DPI function
int32_t dpi_add(int32_t a, int32_t b) {
    int32_t result = a + b;
    printf("[dpi_add] %d + %d = %d\n", a, b, result);
    return result;
}

// Read-write DPI function — reports final result
int32_t dpi_report_result(int32_t passed, int32_t failed) {
    printf("[dpi_report] passed=%d, failed=%d, log_count=%d\n",
           passed, failed, log_count);
    assert(log_count > 0);  // Verify FIFO entries were delivered
    if (failed == 0) {
        printf("[dpi_report] TEST PASSED: %d/%d iterations, %d log entries\n",
               passed, passed + failed, log_count);
    } else {
        printf("[dpi_report] TEST FAILED: %d failures\n", failed);
    }
    return failed;
}
