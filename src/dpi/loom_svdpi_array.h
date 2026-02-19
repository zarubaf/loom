// SPDX-License-Identifier: Apache-2.0
// Loom svOpenArrayHandle internal representation
//
// This struct is what svOpenArrayHandle (void*) actually points to.
// Constructed on the stack by generated dispatch wrappers.

#ifndef LOOM_SVDPI_ARRAY_H
#define LOOM_SVDPI_ARRAY_H

typedef struct {
    void *data;          // pointer to element storage (e.g., uint32_t[])
    int   n_elements;    // number of array elements
    int   elem_width;    // bits per element (e.g., 32 for bit[31:0])
} loom_sv_array_t;

#endif // LOOM_SVDPI_ARRAY_H
