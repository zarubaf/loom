// SPDX-License-Identifier: Apache-2.0
// Socket DPI functions for AXI-Lite BFM
//
// These functions provide a Unix domain socket interface for the BFM.
// The BFM uses these to receive read/write requests from the host and
// send responses back. This is completely DUT-agnostic.

#include <svdpi.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

static int server_fd = -1;
static int client_fd = -1;
static int trace_enabled = 0;

// Wire protocol message types
#define LOOM_SOCK_READ       0
#define LOOM_SOCK_WRITE      1
#define LOOM_SOCK_READ_RESP  0
#define LOOM_SOCK_WRITE_ACK  1
#define LOOM_SOCK_IRQ        2

// Enable/disable trace logging
void loom_sock_set_trace(int enable) {
    trace_enabled = enable;
}

// Initialize socket server, wait for client connection
// Called once at simulation start
int loom_sock_init(const char *path) {
    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("[loom_bfm] socket");
        return -1;
    }

    // Remove any existing socket file
    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[loom_bfm] bind");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    if (listen(server_fd, 1) < 0) {
        perror("[loom_bfm] listen");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    printf("[loom_bfm] Waiting for connection on %s ...\n", path);
    fflush(stdout);

    client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("[loom_bfm] accept");
        close(server_fd);
        server_fd = -1;
        return -1;
    }

    printf("[loom_bfm] Connected\n");
    fflush(stdout);

    // Set non-blocking for polling in try_recv
    fcntl(client_fd, F_SETFL, O_NONBLOCK);
    return 0;
}

// Non-blocking attempt to receive a 12-byte request
// Returns: 1 if request received, 0 if nothing available, -1 on error
int loom_sock_try_recv(
    unsigned char *req_type,
    unsigned int  *req_offset,
    unsigned int  *req_wdata
) {
    if (client_fd < 0) return -1;

    struct pollfd pfd = { .fd = client_fd, .events = POLLIN };
    int poll_ret = poll(&pfd, 1, 0);

    if (poll_ret < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }
    if (poll_ret == 0) return 0;

    // Data available - read the full 12-byte message
    unsigned char buf[12];

    // Temporarily set blocking for complete read
    fcntl(client_fd, F_SETFL, 0);

    ssize_t total = 0;
    while (total < 12) {
        ssize_t n = read(client_fd, buf + total, 12 - total);
        if (n <= 0) {
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            if (n == 0) {
                printf("[loom_bfm] Client disconnected\n");
            }
            return -1;
        }
        total += n;
    }

    fcntl(client_fd, F_SETFL, O_NONBLOCK);

    // Parse little-endian message
    *req_type   = buf[0];
    *req_offset = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
    *req_wdata  = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);

    if (trace_enabled) {
        printf("[DPI] try_recv: type=%d offset=0x%08x wdata=0x%08x\n",
               *req_type, *req_offset, *req_wdata);
        fflush(stdout);
    }

    return 1;
}

// Send a 12-byte response (blocking)
void loom_sock_send(
    unsigned char resp_type,
    unsigned int  rdata,
    unsigned int  irq_bits
) {
    if (client_fd < 0) return;

    unsigned char buf[12] = {0};
    buf[0] = resp_type;
    // bytes 1-3 reserved
    buf[4] = rdata & 0xFF;
    buf[5] = (rdata >> 8) & 0xFF;
    buf[6] = (rdata >> 16) & 0xFF;
    buf[7] = (rdata >> 24) & 0xFF;
    buf[8] = irq_bits & 0xFF;
    buf[9] = (irq_bits >> 8) & 0xFF;
    buf[10] = (irq_bits >> 16) & 0xFF;
    buf[11] = (irq_bits >> 24) & 0xFF;

    // Blocking send
    fcntl(client_fd, F_SETFL, 0);

    size_t total = 0;
    while (total < 12) {
        ssize_t n = write(client_fd, buf + total, 12 - total);
        if (n <= 0) {
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            return;
        }
        total += n;
    }

    fcntl(client_fd, F_SETFL, O_NONBLOCK);
}

// Clean up sockets
void loom_sock_close(void) {
    if (client_fd >= 0) {
        close(client_fd);
        client_fd = -1;
    }
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
}

#ifdef __cplusplus
}
#endif
