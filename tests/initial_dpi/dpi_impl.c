// SPDX-License-Identifier: Apache-2.0
// DPI implementations for initial_dpi_test

#include <svdpi.h>
#include <stdint.h>
#include <stdio.h>

static int init_setup_called = 0;

void init_setup(const char *tag) {
    init_setup_called = 1;
    printf("[dpi] init_setup called with tag: %s\n", tag);
}

int32_t get_init_val(int32_t seed) {
    int32_t result = seed * 0x1234;
    printf("[dpi] get_init_val(%d) = 0x%x\n", seed, result);
    return result;
}
