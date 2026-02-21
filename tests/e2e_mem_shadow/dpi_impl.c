// SPDX-License-Identifier: Apache-2.0
// DPI implementation for mem_shadow E2E test
//
// dpi_checksum: simple function that XORs two values
// Used to verify DPI calls work alongside memory shadow.

#include <stdio.h>
#include <stdint.h>

int32_t dpi_checksum(int32_t a, int32_t b) {
    int32_t result = a ^ b;
    printf("[dpi] dpi_checksum(0x%x, 0x%x) = 0x%x\n", a, b, result);
    return result;
}
