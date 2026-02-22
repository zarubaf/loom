/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal startup for Snitch RV32I — boot from 0x80000000 */

    .section .text.start, "ax", @progbits
    .global _start
    .type   _start, @function

_start:
    /* Set stack pointer to top of SRAM (64 KB) */
    lui     sp, %hi(0x80010000)
    addi    sp, sp, %lo(0x80010000)

    /* Clear BSS */
    la      a0, __bss_start
    la      a1, __bss_end
1:
    bge     a0, a1, 2f
    sw      zero, 0(a0)
    addi    a0, a0, 4
    j       1b
2:

    /* Call main */
    jal     ra, main

    /* Write exit code to exit register (0x10000004) */
    lui     t0, %hi(0x10000004)
    sw      a0, %lo(0x10000004)(t0)

    /* Spin forever (should not reach here) */
3:
    wfi
    j       3b

    .size   _start, .-_start
