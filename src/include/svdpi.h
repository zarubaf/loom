// SPDX-License-Identifier: Apache-2.0
// Minimal svdpi.h — IEEE 1800 DPI type definitions for Loom
//
// Only the subset needed by Loom is implemented. Unsupported functions
// will produce a compile-time or link-time error so we know what to add.

#ifndef SVDPI_H
#define SVDPI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Scalar types (IEEE 1800-2017 H.7.2)
// ============================================================================
typedef uint8_t  svScalar;
typedef svScalar svBit;
typedef svScalar svLogic;

// ============================================================================
// Packed bit/logic vectors (IEEE 1800-2017 H.7.3)
// ============================================================================
typedef uint32_t svBitVecVal;

typedef struct {
    uint32_t aval;
    uint32_t bval;
} svLogicVecVal;

// ============================================================================
// Open array handle (IEEE 1800-2017 H.12)
// ============================================================================
typedef void *svOpenArrayHandle;

// ============================================================================
// Open array access — stubs (will link-error if called)
// ============================================================================

// Get raw pointer to the storage of an open array.
// This is the most commonly used function for open-array DPI arguments.
void *svGetArrayPtr(const svOpenArrayHandle h);

// Array querying
int svLeft(const svOpenArrayHandle h, int d);
int svRight(const svOpenArrayHandle h, int d);
int svLow(const svOpenArrayHandle h, int d);
int svHigh(const svOpenArrayHandle h, int d);
int svLength(const svOpenArrayHandle h, int d);
int svDimensions(const svOpenArrayHandle h);
int svSizeOfArray(const svOpenArrayHandle h);

#ifdef __cplusplus
}
#endif

#endif // SVDPI_H
