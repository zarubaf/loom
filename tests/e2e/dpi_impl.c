// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for dpi_test e2e test

#include <stdio.h>
#include <stdint.h>
#include <vpi_user.h>

// dpi_add: Add two integers
int32_t dpi_add(int32_t a, int32_t b) {
    int32_t result = a + b;
    printf("[dpi] dpi_add(%d, %d) = %d\n", a, b, result);
    return result;
}

// dpi_report_result: Report test result to host
int32_t dpi_report_result(int32_t passed, int32_t result) {
    printf("[dpi] dpi_report_result(passed=%d, result=%d)\n", passed, result);
    if (passed) {
        printf("[dpi] TEST PASSED: result=%d\n", result);
    } else {
        printf("[dpi] TEST FAILED: result=%d\n", result);
    }
    vpi_control(vpiFinish, passed ? 0 : 1);
    return 0;
}
