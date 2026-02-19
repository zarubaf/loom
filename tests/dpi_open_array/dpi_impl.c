// SPDX-License-Identifier: Apache-2.0
// DPI implementation for open array test

#include <svdpi.h>
#include <stdint.h>
#include <stdio.h>

int32_t dpi_fill_array(const char *name, svOpenArrayHandle data, int32_t n_elements) {
    svBitVecVal *ptr = (svBitVecVal *)svGetArrayPtr(data);
    printf("[dpi] dpi_fill_array(\"%s\", n=%d)\n", name, n_elements);
    for (int i = 0; i < n_elements; i++) {
        ptr[i] = (i + 1) * 0x11111111;
        printf("[dpi]   data[%d] = 0x%08x\n", i, ptr[i]);
    }
    return n_elements;
}

int32_t dpi_sum_array(const char *name, svOpenArrayHandle data, int32_t n_elements) {
    const svBitVecVal *ptr = (const svBitVecVal *)svGetArrayPtr(data);
    printf("[dpi] dpi_sum_array(\"%s\", n=%d)\n", name, n_elements);
    uint32_t sum = 0;
    for (int i = 0; i < n_elements; i++) {
        printf("[dpi]   data[%d] = 0x%08x\n", i, ptr[i]);
        sum += ptr[i];
    }
    printf("[dpi]   sum = 0x%08x (%u)\n", sum, sum);
    return (int32_t)sum;
}
