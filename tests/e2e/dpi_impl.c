// SPDX-License-Identifier: Apache-2.0
// DPI function implementations for dpi_test e2e test
//
// These functions implement the DPI interface defined in dpi_test.sv.
// Function names match exactly what the generated header expects.

#include <stdio.h>
#include <stdint.h>
#include "dpi_test_dpi.h"
#include "loom_dpi_service.h"

// ============================================================================
// DPI function implementations
// ============================================================================

// dpi_add: Add two integers
// Called from StCallAdd state in dpi_test.sv
int32_t dpi_add(int32_t a, int32_t b) {
    int32_t result = a + b;
    printf("[dpi] dpi_add(%d, %d) = %d\n", a, b, result);
    return result;
}

// dpi_report_result: Report test result to host
// Called from StReport state in dpi_test.sv
// Args: passed (0 or 1), result (the computed value)
int32_t dpi_report_result(int32_t passed, int32_t result) {
    printf("[dpi] dpi_report_result(passed=%d, result=%d)\n", passed, result);
    if (passed) {
        printf("[dpi] TEST PASSED: result=%d\n", result);
    } else {
        printf("[dpi] TEST FAILED: result=%d\n", result);
    }
    // Signal test completion to the service loop
    loom_dpi_service_request_exit();
    return 0;
}
