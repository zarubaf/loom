// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for dpi_test e2e test
//
// This file implements the DPI functions declared in the generated header.
// Define LOOM_DPI_IMPL before including to get the function table.

#include <stdio.h>
#include "loom_dpi_service.h"

#define LOOM_DPI_IMPL
#include "dpi_test_dpi.h"

// ============================================================================
// DPI function implementations
// ============================================================================

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
    // Signal test completion
    loom_service_finish(passed ? 0 : 1);
    return 0;
}
