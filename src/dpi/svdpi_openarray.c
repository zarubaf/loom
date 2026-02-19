// SPDX-License-Identifier: Apache-2.0
// Loom svdpi open array function implementations (IEEE 1800-2017 H.12)
//
// These implement the svOpenArrayHandle API from svdpi.h.
// The handle points to a loom_sv_array_t struct (stack-allocated by
// generated dispatch wrappers).

#include "svdpi.h"
#include "loom_svdpi_array.h"

void *svGetArrayPtr(const svOpenArrayHandle h) {
    const loom_sv_array_t *arr = (const loom_sv_array_t *)h;
    return arr->data;
}

int svDimensions(const svOpenArrayHandle h) {
    (void)h;
    return 1; // Loom only supports 1-D open arrays
}

int svLeft(const svOpenArrayHandle h, int d) {
    const loom_sv_array_t *arr = (const loom_sv_array_t *)h;
    (void)d;
    return arr->n_elements - 1;
}

int svRight(const svOpenArrayHandle h, int d) {
    (void)h;
    (void)d;
    return 0;
}

int svLow(const svOpenArrayHandle h, int d) {
    (void)h;
    (void)d;
    return 0;
}

int svHigh(const svOpenArrayHandle h, int d) {
    const loom_sv_array_t *arr = (const loom_sv_array_t *)h;
    (void)d;
    return arr->n_elements - 1;
}

int svLength(const svOpenArrayHandle h, int d) {
    const loom_sv_array_t *arr = (const loom_sv_array_t *)h;
    (void)d;
    return arr->n_elements;
}

int svSizeOfArray(const svOpenArrayHandle h) {
    const loom_sv_array_t *arr = (const loom_sv_array_t *)h;
    return arr->n_elements * ((arr->elem_width + 7) / 8);
}
