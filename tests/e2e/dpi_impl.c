// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for dpi_test e2e test

#include <stdio.h>
#include <stdint.h>

// Global tracking for test harness
static int g_test_passed = -1;  // -1 = not called, 0 = fail, 1 = pass
static uint32_t g_test_result = 0;

// Accessors for test harness
int dpi_get_test_passed(void) { return g_test_passed; }
uint32_t dpi_get_test_result(void) { return g_test_result; }

// DPI function: dpi_add (func_id=0)
uint32_t impl_dpi_add(uint32_t a, uint32_t b) {
    uint32_t result = a + b;
    printf("[dpi] dpi_add(%u, %u) = %u\n", a, b, result);
    return result;
}

// DPI function: dpi_report_result (func_id=1)
uint32_t impl_dpi_report_result(uint32_t passed, uint32_t result) {
    printf("[dpi] dpi_report_result(passed=%u, result=%u)\n", passed, result);
    g_test_passed = passed ? 1 : 0;
    g_test_result = result;
    // Return 0 to indicate success
    return 0;
}
