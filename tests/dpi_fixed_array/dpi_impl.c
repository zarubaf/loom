// SPDX-License-Identifier: Apache-2.0
// DPI implementation for fixed-size array test
//
// Fixed-size unpacked arrays map to plain pointers in the DPI-C convention
// (unlike open arrays which use svOpenArrayHandle).

#include <stdint.h>
#include <stdio.h>

int32_t dpi_fill_array(const char *name, uint32_t *data, int32_t n_elements) {
    printf("[dpi] dpi_fill_array(\"%s\", n=%d)\n", name, n_elements);
    for (int i = 0; i < n_elements; i++) {
        data[i] = (i + 1) * 0x11111111;
        printf("[dpi]   data[%d] = 0x%08x\n", i, data[i]);
    }
    return n_elements;
}

int32_t dpi_sum_array(const char *name, const uint32_t *data, int32_t n_elements) {
    printf("[dpi] dpi_sum_array(\"%s\", n=%d)\n", name, n_elements);
    uint32_t sum = 0;
    for (int i = 0; i < n_elements; i++) {
        printf("[dpi]   data[%d] = 0x%08x\n", i, data[i]);
        sum += data[i];
    }
    printf("[dpi]   sum = 0x%08x (%u)\n", sum, sum);
    return (int32_t)sum;
}
