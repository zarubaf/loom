// SPDX-License-Identifier: Apache-2.0
// DPI host I/O for Snitch Hello World demo
//
// Memory-mapped I/O:
//   0x1000_0000  UART data (write -> putchar, read -> status)
//   0x1000_0004  Exit register (write -> vpi_control finish)
//   0x1000_0008  Host scratch (write stores, read returns *previous* value)

#include <stdio.h>
#include <stdint.h>
#include "vpi_user.h"

static uint32_t scratch_prev = 0;
static uint32_t scratch_cur  = 0;

int32_t dpi_host_read(int32_t addr) {
    uint32_t uaddr = (uint32_t)addr;

    switch (uaddr) {
    case 0x10000000:
        return 1;  // UART status: always ready
    case 0x10000008:
        return (int32_t)scratch_prev;  // swap semantics
    default:
        return 0;
    }
}

void dpi_host_write(int32_t addr, int32_t wdata, int32_t strb) {
    uint32_t uaddr = (uint32_t)addr;

    switch (uaddr) {
    case 0x10000000:
        putchar(wdata & 0xFF);
        fflush(stdout);
        break;
    case 0x10000004:
        printf("Host received exit code: 0x%08x\n", wdata);
        vpi_control(vpiFinish, wdata & 0xFF);
        break;
    case 0x10000008:
        scratch_prev = scratch_cur;
        scratch_cur  = (uint32_t)wdata;
        break;
    }
}
