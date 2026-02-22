// SPDX-License-Identifier: Apache-2.0
// Standalone C test driver for loom_axil_firewall testbench
//
// Connects to the BFM socket, runs 12 test functions, exits.
// Exit code 0 = all tests passed.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

// =========================================================================
// Socket Protocol (matches loom_sock_dpi.c)
// =========================================================================
// Send 12B: [type:1B | reserved:3B | offset:4B | wdata:4B]
// Recv 12B: [type:1B | reserved:3B | rdata:4B  | irq:4B  ]
//
// Types: 0=read/read_resp, 1=write/write_ack, 2=IRQ, 3=SHUTDOWN

#define MSG_READ       0
#define MSG_WRITE      1
#define MSG_READ_RESP  0
#define MSG_WRITE_ACK  1
#define MSG_IRQ        2
#define MSG_SHUTDOWN   3

// =========================================================================
// Address macros (20-bit address space)
// =========================================================================
#define FW_DATA(off)    (0x00000 + (off))   // Firewall data path (s_axi)
#define FW_MGMT(off)    (0x10000 + (off))   // Firewall management (s_mgmt)
#define SLAVE_CTRL(off) (0x20000 + (off))   // Test slave control

// Firewall management register offsets
#define MGMT_CTRL              0x00
#define MGMT_STATUS            0x04
#define MGMT_TIMEOUT_CYCLES    0x08
#define MGMT_RESP_ON_TIMEOUT   0x0C
#define MGMT_RDATA_ON_TIMEOUT  0x10
#define MGMT_TIMEOUT_COUNT     0x14
#define MGMT_UNSOLICITED_COUNT 0x18
#define MGMT_MAX_OUTSTANDING   0x1C
#define MGMT_IRQ_ENABLE        0x20

// Slave control register offsets
#define SLAVE_MODE    0x00
#define SLAVE_DELAY   0x04
#define SLAVE_RDATA   0x08
#define SLAVE_PENDING 0x0C
#define SLAVE_QUIT    0x10

// CTRL register bits
#define CTRL_LOCKDOWN     (1 << 0)
#define CTRL_CLEAR_COUNTS (1 << 1)
#define CTRL_DECOUPLE     (1 << 2)

// STATUS register bits
#define STATUS_LOCKED         (1 << 0)
#define STATUS_WR_OUTSTANDING (1 << 1)
#define STATUS_RD_OUTSTANDING (1 << 2)
#define STATUS_DECOUPLE       (1 << 3)

static int sock_fd = -1;
static int test_pass = 0;
static int test_fail = 0;
static int irq_count = 0;

// =========================================================================
// Socket Helpers
// =========================================================================

static int sock_connect(const char *path) {
    struct sockaddr_un addr;
    int attempts = 50;  // 5 seconds total (100ms intervals)

    while (attempts-- > 0) {
        sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock_fd < 0) {
            perror("socket");
            return -1;
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

        if (connect(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            printf("[driver] Connected to %s\n", path);
            return 0;
        }

        close(sock_fd);
        sock_fd = -1;
        usleep(100000);  // 100ms
    }

    fprintf(stderr, "[driver] Failed to connect to %s after 5s\n", path);
    return -1;
}

static int sock_send(uint8_t type, uint32_t offset, uint32_t wdata) {
    uint8_t buf[12] = {0};
    buf[0] = type;
    buf[4] = offset & 0xFF;
    buf[5] = (offset >> 8) & 0xFF;
    buf[6] = (offset >> 16) & 0xFF;
    buf[7] = (offset >> 24) & 0xFF;
    buf[8] = wdata & 0xFF;
    buf[9] = (wdata >> 8) & 0xFF;
    buf[10] = (wdata >> 16) & 0xFF;
    buf[11] = (wdata >> 24) & 0xFF;

    ssize_t total = 0;
    while (total < 12) {
        ssize_t n = write(sock_fd, buf + total, 12 - total);
        if (n <= 0) return -1;
        total += n;
    }
    return 0;
}

static int sock_recv(uint8_t *type, uint32_t *rdata, uint32_t *irq) {
    uint8_t buf[12];
    ssize_t total = 0;
    while (total < 12) {
        ssize_t n = read(sock_fd, buf + total, 12 - total);
        if (n <= 0) return -1;
        total += n;
    }
    *type = buf[0];
    *rdata = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
    *irq   = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);
    return 0;
}

// Read an AXI register. Skips any IRQ messages.
static uint32_t axi_read(uint32_t addr) {
    if (sock_send(MSG_READ, addr, 0) < 0) {
        fprintf(stderr, "[driver] send failed\n");
        exit(1);
    }
    while (1) {
        uint8_t type;
        uint32_t rdata, irq;
        if (sock_recv(&type, &rdata, &irq) < 0) {
            fprintf(stderr, "[driver] recv failed\n");
            exit(1);
        }
        if (type == MSG_IRQ) {
            irq_count++;
            continue;
        }
        if (type == MSG_SHUTDOWN) {
            fprintf(stderr, "[driver] unexpected SHUTDOWN\n");
            exit(1);
        }
        return rdata;
    }
}

// Write an AXI register. Skips any IRQ messages.
static void axi_write(uint32_t addr, uint32_t data) {
    if (sock_send(MSG_WRITE, addr, data) < 0) {
        fprintf(stderr, "[driver] send failed\n");
        exit(1);
    }
    while (1) {
        uint8_t type;
        uint32_t rdata, irq;
        if (sock_recv(&type, &rdata, &irq) < 0) {
            fprintf(stderr, "[driver] recv failed\n");
            exit(1);
        }
        if (type == MSG_IRQ) {
            irq_count++;
            continue;
        }
        if (type == MSG_SHUTDOWN) {
            fprintf(stderr, "[driver] unexpected SHUTDOWN\n");
            exit(1);
        }
        return;
    }
}

// Check for pending IRQ messages (non-blocking via a short-timeout approach).
// Since we can't do non-blocking on the socket easily, we just drain IRQs
// that arrived between commands using the irq_count that axi_read/write track.

// =========================================================================
// Test Assertions
// =========================================================================

#define CHECK(cond, fmt, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: " fmt "\n", ##__VA_ARGS__); \
        test_fail++; \
        return; \
    } \
} while(0)

#define CHECK_EQ(actual, expected, name) do { \
    uint32_t _a = (actual), _e = (expected); \
    if (_a != _e) { \
        printf("  FAIL: %s = 0x%08x, expected 0x%08x\n", name, _a, _e); \
        test_fail++; \
        return; \
    } \
} while(0)

// =========================================================================
// Test Suite
// =========================================================================

// Helper: advance simulation by doing N dummy mgmt reads
static void spin_cycles(int n) {
    for (int i = 0; i < n; i++) {
        axi_read(FW_MGMT(MGMT_STATUS));
    }
}

// Helper: drain slave and wait until pending count reaches 0
static void drain_slave(void) {
    axi_write(SLAVE_CTRL(SLAVE_MODE), 2);  // DRAIN mode
    for (int i = 0; i < 200; i++) {
        uint32_t pending = axi_read(SLAVE_CTRL(SLAVE_PENDING));
        if (pending == 0) break;
    }
    // Extra cycles so firewall processes any unsolicited responses
    spin_cycles(5);
}

// Helper: reset firewall state by clearing counts and lockdown
static void reset_firewall_state(void) {
    // First ensure slave is in normal mode (drain any pending)
    axi_write(SLAVE_CTRL(SLAVE_MODE), 0);
    axi_write(SLAVE_CTRL(SLAVE_DELAY), 0);
    axi_write(SLAVE_CTRL(SLAVE_RDATA), 0xCAFEBABE);
    // Let any in-flight downstream responses settle
    spin_cycles(5);
    // Clear lockdown & decouple, clear counts
    axi_write(FW_MGMT(MGMT_CTRL), CTRL_CLEAR_COUNTS);
    spin_cycles(2);
    // Clear lockdown (clear_counts is W1C, so just write 0 now)
    axi_write(FW_MGMT(MGMT_CTRL), 0);
    // Restore default timeout config
    axi_write(FW_MGMT(MGMT_TIMEOUT_CYCLES), 50);
    axi_write(FW_MGMT(MGMT_RESP_ON_TIMEOUT), 0x00000002);
    axi_write(FW_MGMT(MGMT_RDATA_ON_TIMEOUT), 0xDEADBEEF);
    axi_write(FW_MGMT(MGMT_MAX_OUTSTANDING), 4);
    axi_write(FW_MGMT(MGMT_IRQ_ENABLE), 0);
    spin_cycles(2);
}

// Test 1: Read all management registers and verify reset values
static void test_register_defaults(void) {
    printf("[test 1] register_defaults\n");

    uint32_t ctrl    = axi_read(FW_MGMT(MGMT_CTRL));
    uint32_t status  = axi_read(FW_MGMT(MGMT_STATUS));
    uint32_t timeout = axi_read(FW_MGMT(MGMT_TIMEOUT_CYCLES));
    uint32_t resp    = axi_read(FW_MGMT(MGMT_RESP_ON_TIMEOUT));
    uint32_t rdata   = axi_read(FW_MGMT(MGMT_RDATA_ON_TIMEOUT));
    uint32_t tcount  = axi_read(FW_MGMT(MGMT_TIMEOUT_COUNT));
    uint32_t ucount  = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));
    uint32_t maxout  = axi_read(FW_MGMT(MGMT_MAX_OUTSTANDING));
    uint32_t irqen   = axi_read(FW_MGMT(MGMT_IRQ_ENABLE));

    CHECK_EQ(ctrl,    0x00000000, "CTRL");
    CHECK_EQ(status,  0x00000000, "STATUS");
    CHECK_EQ(timeout, 50,         "TIMEOUT_CYCLES");
    CHECK_EQ(resp,    0x00000002, "RESP_ON_TIMEOUT");
    CHECK_EQ(rdata,   0xDEADBEEF, "RDATA_ON_TIMEOUT");
    CHECK_EQ(tcount,  0x00000000, "TIMEOUT_COUNT");
    CHECK_EQ(ucount,  0x00000000, "UNSOLICITED_COUNT");
    CHECK_EQ(maxout,  4,          "MAX_OUTSTANDING");
    CHECK_EQ(irqen,   0x00000000, "IRQ_ENABLE");

    printf("  PASS\n");
    test_pass++;
}

// Test 2: Write/readback writable registers with non-default values
static void test_register_readback(void) {
    printf("[test 2] register_readback\n");

    axi_write(FW_MGMT(MGMT_TIMEOUT_CYCLES), 100);
    CHECK_EQ(axi_read(FW_MGMT(MGMT_TIMEOUT_CYCLES)), 100, "TIMEOUT_CYCLES");

    axi_write(FW_MGMT(MGMT_RESP_ON_TIMEOUT), 0x00000003);
    CHECK_EQ(axi_read(FW_MGMT(MGMT_RESP_ON_TIMEOUT)), 0x00000003, "RESP_ON_TIMEOUT");

    axi_write(FW_MGMT(MGMT_RDATA_ON_TIMEOUT), 0x12345678);
    CHECK_EQ(axi_read(FW_MGMT(MGMT_RDATA_ON_TIMEOUT)), 0x12345678, "RDATA_ON_TIMEOUT");

    axi_write(FW_MGMT(MGMT_MAX_OUTSTANDING), 8);
    CHECK_EQ(axi_read(FW_MGMT(MGMT_MAX_OUTSTANDING)), 8, "MAX_OUTSTANDING");

    axi_write(FW_MGMT(MGMT_IRQ_ENABLE), 0x00000003);
    CHECK_EQ(axi_read(FW_MGMT(MGMT_IRQ_ENABLE)), 0x00000003, "IRQ_ENABLE");

    // Restore defaults
    axi_write(FW_MGMT(MGMT_TIMEOUT_CYCLES), 50);
    axi_write(FW_MGMT(MGMT_RESP_ON_TIMEOUT), 0x00000002);
    axi_write(FW_MGMT(MGMT_RDATA_ON_TIMEOUT), 0xDEADBEEF);
    axi_write(FW_MGMT(MGMT_MAX_OUTSTANDING), 4);
    axi_write(FW_MGMT(MGMT_IRQ_ENABLE), 0);

    printf("  PASS\n");
    test_pass++;
}

// Test 3: Normal read/write through firewall with responsive slave
static void test_normal_read_write(void) {
    printf("[test 3] normal_read_write\n");

    reset_firewall_state();

    // Set slave to return specific data
    axi_write(SLAVE_CTRL(SLAVE_RDATA), 0xA5A5A5A5);

    // Read through firewall
    uint32_t rd = axi_read(FW_DATA(0x0000));
    CHECK_EQ(rd, 0xA5A5A5A5, "read data");

    // Write through firewall (just verify it completes without timeout)
    axi_write(FW_DATA(0x0004), 0x12345678);

    // Verify no timeouts occurred
    uint32_t tcount = axi_read(FW_MGMT(MGMT_TIMEOUT_COUNT));
    CHECK_EQ(tcount, 0, "TIMEOUT_COUNT after normal ops");

    printf("  PASS\n");
    test_pass++;
}

// Test 4: Read timeout — stall slave, read → synthetic response
static void test_read_timeout(void) {
    printf("[test 4] read_timeout\n");

    reset_firewall_state();

    // Put slave in STALL mode
    axi_write(SLAVE_CTRL(SLAVE_MODE), 1);

    // Read through firewall — slave won't respond, firewall will timeout
    uint32_t rd = axi_read(FW_DATA(0x0000));
    // Should get synthetic response with RDATA_ON_TIMEOUT (0xDEADBEEF)
    CHECK_EQ(rd, 0xDEADBEEF, "synthetic read data");

    // Check timeout count incremented
    uint32_t tcount = axi_read(FW_MGMT(MGMT_TIMEOUT_COUNT));
    CHECK(tcount >= 1, "TIMEOUT_COUNT = %u, expected >= 1", tcount);

    // Drain the stalled response from the slave
    drain_slave();

    printf("  PASS\n");
    test_pass++;
}

// Test 5: Write timeout — stall slave, write → synthetic BRESP
static void test_write_timeout(void) {
    printf("[test 5] write_timeout\n");

    reset_firewall_state();

    // Put slave in STALL mode
    axi_write(SLAVE_CTRL(SLAVE_MODE), 1);

    // Write through firewall — slave won't respond, firewall will timeout
    axi_write(FW_DATA(0x0000), 0x11223344);

    // Check timeout count
    uint32_t tcount = axi_read(FW_MGMT(MGMT_TIMEOUT_COUNT));
    CHECK(tcount >= 1, "TIMEOUT_COUNT = %u, expected >= 1", tcount);

    // Drain
    drain_slave();

    printf("  PASS\n");
    test_pass++;
}

// Test 6: Unsolicited after timeout — stall→timeout→drain→check UNSOLICITED_COUNT
static void test_unsolicited_after_timeout(void) {
    printf("[test 6] unsolicited_after_timeout\n");

    reset_firewall_state();

    // Stall slave, trigger a read timeout
    axi_write(SLAVE_CTRL(SLAVE_MODE), 1);
    axi_read(FW_DATA(0x0000));  // triggers timeout

    // Now drain — the slave will send back a response that has no matching
    // outstanding (firewall already generated synthetic), so it's unsolicited
    drain_slave();

    uint32_t ucount = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));
    CHECK(ucount >= 1, "UNSOLICITED_COUNT = %u, expected >= 1", ucount);

    printf("  PASS\n");
    test_pass++;
}

// Test 7: Force unsolicited — slave MODE=3/4, verify UNSOLICITED_COUNT
static void test_force_unsolicited(void) {
    printf("[test 7] force_unsolicited\n");

    reset_firewall_state();

    uint32_t ucount_before = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));

    // Trigger unsolicited read response
    axi_write(SLAVE_CTRL(SLAVE_MODE), 3);  // UNSOL_RD
    // Advance sim cycles so the pulse propagates through firewall
    spin_cycles(10);

    uint32_t ucount_after_rd = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));
    CHECK(ucount_after_rd > ucount_before,
          "UNSOLICITED_COUNT after unsol_rd: %u, before: %u", ucount_after_rd, ucount_before);

    // Trigger unsolicited write response
    axi_write(SLAVE_CTRL(SLAVE_MODE), 4);  // UNSOL_WR
    spin_cycles(10);

    uint32_t ucount_after_wr = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));
    CHECK(ucount_after_wr > ucount_after_rd,
          "UNSOLICITED_COUNT after unsol_wr: %u, after unsol_rd: %u", ucount_after_wr, ucount_after_rd);

    printf("  PASS\n");
    test_pass++;
}

// Test 8: Lockdown — set/clear CTRL.lockdown, verify STATUS.locked
static void test_lockdown(void) {
    printf("[test 8] lockdown\n");

    reset_firewall_state();

    // Enable lockdown
    axi_write(FW_MGMT(MGMT_CTRL), CTRL_LOCKDOWN);

    // Read STATUS — should show locked
    uint32_t status = axi_read(FW_MGMT(MGMT_STATUS));
    CHECK(status & STATUS_LOCKED, "STATUS.locked not set after lockdown enable (0x%08x)", status);

    // Clear lockdown
    axi_write(FW_MGMT(MGMT_CTRL), 0);

    status = axi_read(FW_MGMT(MGMT_STATUS));
    CHECK(!(status & STATUS_LOCKED), "STATUS.locked still set after lockdown disable (0x%08x)", status);

    // Verify normal operation works after clearing lockdown
    // (reset_firewall_state already set slave RDATA to 0xCAFEBABE)
    uint32_t rd = axi_read(FW_DATA(0x0000));
    CHECK_EQ(rd, 0xCAFEBABE, "read after lockdown clear");

    printf("  PASS\n");
    test_pass++;
}

// Test 9: Decouple — set/clear CTRL.decouple, verify STATUS.decouple_status
static void test_decouple(void) {
    printf("[test 9] decouple\n");

    reset_firewall_state();

    // Enable decouple via CTRL
    axi_write(FW_MGMT(MGMT_CTRL), CTRL_DECOUPLE);

    uint32_t status = axi_read(FW_MGMT(MGMT_STATUS));
    CHECK(status & STATUS_DECOUPLE, "STATUS.decouple not set (0x%08x)", status);

    // Clear decouple
    axi_write(FW_MGMT(MGMT_CTRL), 0);

    status = axi_read(FW_MGMT(MGMT_STATUS));
    CHECK(!(status & STATUS_DECOUPLE), "STATUS.decouple still set (0x%08x)", status);

    printf("  PASS\n");
    test_pass++;
}

// Test 10: Clear counts — generate counts, write CTRL.clear_counts, verify zeroed
static void test_clear_counts(void) {
    printf("[test 10] clear_counts\n");

    reset_firewall_state();

    // Generate a timeout to create non-zero counts
    axi_write(SLAVE_CTRL(SLAVE_MODE), 1);  // STALL
    axi_read(FW_DATA(0x0000));             // triggers timeout

    // Drain the stalled response (also generates unsolicited count)
    drain_slave();

    // Verify we have some counts
    uint32_t tcount = axi_read(FW_MGMT(MGMT_TIMEOUT_COUNT));
    uint32_t ucount = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));
    CHECK(tcount > 0 || ucount > 0,
          "No counts to clear (timeout=%u, unsol=%u)", tcount, ucount);

    // Clear counts (W1C bit)
    axi_write(FW_MGMT(MGMT_CTRL), CTRL_CLEAR_COUNTS);
    // The write also sets clear_counts but that self-clears
    // Ensure lockdown is not set
    axi_write(FW_MGMT(MGMT_CTRL), 0);

    tcount = axi_read(FW_MGMT(MGMT_TIMEOUT_COUNT));
    ucount = axi_read(FW_MGMT(MGMT_UNSOLICITED_COUNT));
    CHECK_EQ(tcount, 0, "TIMEOUT_COUNT after clear");
    CHECK_EQ(ucount, 0, "UNSOLICITED_COUNT after clear");

    printf("  PASS\n");
    test_pass++;
}

// Test 11: Custom timeout response — change RESP/RDATA, verify used
static void test_custom_timeout_response(void) {
    printf("[test 11] custom_timeout_response\n");

    reset_firewall_state();

    // Set custom timeout response values
    axi_write(FW_MGMT(MGMT_RDATA_ON_TIMEOUT), 0xBADC0FFE);
    axi_write(FW_MGMT(MGMT_RESP_ON_TIMEOUT), 0x00000003);  // DECERR

    // Stall slave and trigger read timeout
    axi_write(SLAVE_CTRL(SLAVE_MODE), 1);
    uint32_t rd = axi_read(FW_DATA(0x0000));
    CHECK_EQ(rd, 0xBADC0FFE, "custom synthetic read data");

    // Drain
    drain_slave();

    printf("  PASS\n");
    test_pass++;
}

// Test 12: IRQ — enable IRQs, trigger timeout, check IRQ message arrives
static void test_irq(void) {
    printf("[test 12] irq\n");

    reset_firewall_state();

    // Reset IRQ counter
    irq_count = 0;

    // Enable timeout IRQ
    axi_write(FW_MGMT(MGMT_IRQ_ENABLE), 0x00000001);

    // Stall slave, trigger timeout
    axi_write(SLAVE_CTRL(SLAVE_MODE), 1);
    axi_read(FW_DATA(0x0000));  // triggers timeout + IRQ

    // The IRQ message should have been received while reading the
    // timeout response (skipped by axi_read, counted in irq_count)
    CHECK(irq_count >= 1, "irq_count = %d, expected >= 1", irq_count);

    // Cleanup: drain and disable IRQs
    drain_slave();
    axi_write(FW_MGMT(MGMT_IRQ_ENABLE), 0);

    printf("  PASS\n");
    test_pass++;
}

// =========================================================================
// Main
// =========================================================================

int main(int argc, char *argv[]) {
    const char *socket_path = "/tmp/fw_test.sock";

    if (argc > 1) {
        socket_path = argv[1];
    }

    printf("[driver] Connecting to %s\n", socket_path);
    if (sock_connect(socket_path) < 0) {
        return 1;
    }

    printf("[driver] Running firewall tests\n");
    printf("=========================================\n");

    test_register_defaults();
    test_register_readback();
    test_normal_read_write();
    test_read_timeout();
    test_write_timeout();
    test_unsolicited_after_timeout();
    test_force_unsolicited();
    test_lockdown();
    test_decouple();
    test_clear_counts();
    test_custom_timeout_response();
    test_irq();

    printf("=========================================\n");
    printf("[driver] Results: %d passed, %d failed\n", test_pass, test_fail);

    // Shutdown: write QUIT register on test slave
    printf("[driver] Sending QUIT\n");
    axi_write(SLAVE_CTRL(SLAVE_QUIT), 1);

    // Wait for SHUTDOWN message from BFM
    uint8_t type;
    uint32_t rdata, irq;
    while (1) {
        if (sock_recv(&type, &rdata, &irq) < 0) break;
        if (type == MSG_SHUTDOWN) break;
    }

    close(sock_fd);
    return test_fail > 0 ? 1 : 0;
}
