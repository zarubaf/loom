// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for dpi_test e2e test
//
// These are pure user functions - no Loom-specific includes needed.
// The hardware controls simulation termination via $finish.

#include <stdio.h>
#include <stdint.h>

// multisim_server_start: Initialize server with given name
void multisim_server_start(const char *server_name) {
    printf("[dpi] multisim_server_start(\"%s\")\n", server_name);
}

// dpi_add: Add two integers
int32_t dpi_add(int32_t a, int32_t b) {
    int32_t result = a + b;
    printf("[dpi] dpi_add(%d, %d) = %d\n", a, b, result);
    return result;
}

// dpi_report_result: Report final test result
//   passed = number of passing iterations
//   result = number of failing iterations
int32_t dpi_report_result(int32_t passed, int32_t failed) {
    printf("[dpi] dpi_report_result(passed=%d, failed=%d)\n", passed, failed);
    if (failed == 0) {
        printf("[dpi] TEST PASSED: %d/%d iterations OK\n", passed, passed + failed);
    } else {
        printf("[dpi] TEST FAILED: %d failures out of %d iterations\n",
               failed, passed + failed);
    }
    return failed;
}
