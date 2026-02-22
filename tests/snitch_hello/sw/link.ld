/* SPDX-License-Identifier: Apache-2.0 */
/* Linker script for Snitch Hello World — 64 KB SRAM at 0x80000000 */

OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    SRAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64K
}

SECTIONS
{
    .text : {
        *(.text.start)
        *(.text .text.*)
    } > SRAM

    .rodata : {
        *(.rodata .rodata.*)
    } > SRAM

    .data : {
        *(.data .data.*)
    } > SRAM

    .bss (NOLOAD) : ALIGN(4) {
        . = ALIGN(4);
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > SRAM

    /* Stack grows downward from top of SRAM */
    _stack_top = ORIGIN(SRAM) + LENGTH(SRAM);
}
