// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for dpi_test e2e test

#include <stdio.h>
#include <stdint.h>

// Global tracking for test harness
static int g_dpi_calls = 0;

// Accessors for test harness
int dpi_get_test_passed(void) {
    // Test passes if at least one DPI call was made
    // Actual pass/fail is checked by simulation via test_pass_o
    return g_dpi_calls > 0 ? 1 : -1;
}
uint32_t dpi_get_test_result(void) { return 0; }

// DPI function: dpi_add
uint32_t impl_dpi_add(uint32_t a, uint32_t b) {
    uint32_t result = a + b;
    printf("[dpi] dpi_add(%u, %u) = %u\n", a, b, result);
    g_dpi_calls++;
    return result;
}
