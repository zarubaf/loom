// SPDX-License-Identifier: Apache-2.0
// DPI implementations for scan_dump_test
//
// Four DPI call types:
//   dpi_add        — scalar with return (int + int -> int)
//   dpi_notify     — void (no return value)
//   dpi_fill_fixed — fixed-size output array (plain pointer on C side)
//   dpi_sum_open   — open-array input (svOpenArrayHandle on C side)

#include <svdpi.h>
#include <stdint.h>
#include <stdio.h>

int32_t dpi_add(int32_t a, int32_t b) {
    int32_t result = a + b;
    printf("[dpi] dpi_add(%d, %d) = %d (0x%x)\n", a, b, result, result);
    return result;
}

void dpi_notify(int32_t value) {
    printf("[dpi] dpi_notify(%d)\n", value);
}

int32_t dpi_fill_fixed(const char *name, uint32_t *data, int32_t n) {
    printf("[dpi] dpi_fill_fixed(\"%s\", n=%d)\n", name, n);
    for (int i = 0; i < n; i++) {
        data[i] = (uint32_t)(i + 1) * 0x11111111u;
        printf("[dpi]   data[%d] = 0x%08x\n", i, data[i]);
    }
    return n;
}

int32_t dpi_sum_open(const char *name, svOpenArrayHandle data, int32_t n) {
    const svBitVecVal *ptr = (const svBitVecVal *)svGetArrayPtr(data);
    printf("[dpi] dpi_sum_open(\"%s\", n=%d)\n", name, n);
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        printf("[dpi]   data[%d] = 0x%08x\n", i, ptr[i]);
        sum += ptr[i];
    }
    printf("[dpi]   sum = 0x%08x\n", sum);
    return (int32_t)sum;
}
