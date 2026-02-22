/* SPDX-License-Identifier: Apache-2.0 */
/* Hello World for Snitch — host I/O via memory-mapped DPI */

#define UART_DATA    (*(volatile unsigned int *)0x10000000)
#define EXIT_REG     (*(volatile unsigned int *)0x10000004)
#define HOST_SCRATCH (*(volatile unsigned int *)0x10000008)

static void putchar_uart(char c) {
    UART_DATA = (unsigned int)c;
}

static void puts_uart(const char *s) {
    while (*s)
        putchar_uart(*s++);
}

int main(void) {
    puts_uart("Snitch reporting for duty 🫡!\n");

    /* Host scratch swap: each read returns the *previous* written value.
     * Write A, write B, read -> get A (the value before B). */
    HOST_SCRATCH = 0xCAFEBABE;
    HOST_SCRATCH = 0xDEADBEEF;
    unsigned int swapped = HOST_SCRATCH;  /* should return 0xCAFEBABE */

    if (swapped == 0xCAFEBABE) {
        puts_uart("PASS: host swap OK\n");
    } else {
        puts_uart("FAIL: host swap\n");
        return 1;
    }
    return 0;
}
