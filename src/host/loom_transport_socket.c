// SPDX-License-Identifier: Apache-2.0
// libloom socket transport implementation
//
// This transport connects to a Verilator simulation via Unix domain socket.
// The wire protocol uses 12-byte fixed-size messages:
//
// Request (host -> sim):
//   [0]     : type (0=read, 1=write)
//   [1-3]   : reserved
//   [4-7]   : address (little-endian)
//   [8-11]  : write data (little-endian, ignored for reads)
//
// Response (sim -> host):
//   [0]     : type (0=read response, 1=write ack, 2=irq)
//   [1-3]   : reserved
//   [4-7]   : read data (little-endian)
//   [8-11]  : irq bits (little-endian)

#include "libloom.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

// Message types
#define MSG_READ        0
#define MSG_WRITE       1
#define MSG_READ_RESP   0
#define MSG_WRITE_ACK   1
#define MSG_IRQ         2

// Private data for socket transport
typedef struct {
    int fd;
    uint32_t pending_irq;  // Accumulated IRQ bits from async messages
} socket_priv_t;

// ============================================================================
// Helper functions
// ============================================================================

static int send_message(int fd, uint8_t type, uint32_t addr, uint32_t wdata) {
    uint8_t buf[12] = {0};
    buf[0] = type;
    // bytes 1-3 reserved
    buf[4] = addr & 0xFF;
    buf[5] = (addr >> 8) & 0xFF;
    buf[6] = (addr >> 16) & 0xFF;
    buf[7] = (addr >> 24) & 0xFF;
    buf[8] = wdata & 0xFF;
    buf[9] = (wdata >> 8) & 0xFF;
    buf[10] = (wdata >> 16) & 0xFF;
    buf[11] = (wdata >> 24) & 0xFF;

    size_t total = 0;
    while (total < 12) {
        ssize_t n = write(fd, buf + total, 12 - total);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return LOOM_ERR_TRANSPORT;
        }
        total += n;
    }
    return LOOM_OK;
}

static int recv_message(int fd, uint8_t *type, uint32_t *rdata, uint32_t *irq_bits) {
    uint8_t buf[12];
    size_t total = 0;
    while (total < 12) {
        ssize_t n = read(fd, buf + total, 12 - total);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return LOOM_ERR_TRANSPORT;
        }
        total += n;
    }

    *type = buf[0];
    *rdata = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
    *irq_bits = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);

    return LOOM_OK;
}

// ============================================================================
// Transport operations
// ============================================================================

static int socket_connect(loom_transport_t *t, const char *target) {
    socket_priv_t *priv = (socket_priv_t *)t->priv;

    // If already connected, just return success
    if (priv->fd >= 0) {
        return LOOM_OK;
    }

    priv->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (priv->fd < 0) {
        perror("[libloom] socket");
        return LOOM_ERR_TRANSPORT;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, target, sizeof(addr.sun_path) - 1);

    if (connect(priv->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[libloom] connect");
        close(priv->fd);
        priv->fd = -1;
        return LOOM_ERR_TRANSPORT;
    }

    printf("[libloom] Connected to %s\n", target);
    return LOOM_OK;
}

static void socket_disconnect(loom_transport_t *t) {
    socket_priv_t *priv = (socket_priv_t *)t->priv;
    if (priv->fd >= 0) {
        close(priv->fd);
        priv->fd = -1;
    }
}

static int socket_read32(loom_transport_t *t, uint32_t addr, uint32_t *data) {
    socket_priv_t *priv = (socket_priv_t *)t->priv;
    if (priv->fd < 0) return LOOM_ERR_NOT_CONNECTED;

    int rc = send_message(priv->fd, MSG_READ, addr, 0);
    if (rc != LOOM_OK) return rc;

    // Wait for response, handling any IRQ messages that arrive first
    while (1) {
        uint8_t type;
        uint32_t rdata, irq_bits;
        rc = recv_message(priv->fd, &type, &rdata, &irq_bits);
        if (rc != LOOM_OK) return rc;

        if (type == MSG_IRQ) {
            // Accumulate IRQ bits for later polling
            priv->pending_irq |= irq_bits;
            continue;
        }

        if (type == MSG_READ_RESP) {
            *data = rdata;
            return LOOM_OK;
        }

        // Unexpected message type
        return LOOM_ERR_PROTOCOL;
    }
}

static int socket_write32(loom_transport_t *t, uint32_t addr, uint32_t data) {
    socket_priv_t *priv = (socket_priv_t *)t->priv;
    if (priv->fd < 0) return LOOM_ERR_NOT_CONNECTED;

    int rc = send_message(priv->fd, MSG_WRITE, addr, data);
    if (rc != LOOM_OK) return rc;

    // Wait for ack, handling any IRQ messages that arrive first
    while (1) {
        uint8_t type;
        uint32_t rdata, irq_bits;
        rc = recv_message(priv->fd, &type, &rdata, &irq_bits);
        if (rc != LOOM_OK) return rc;

        if (type == MSG_IRQ) {
            priv->pending_irq |= irq_bits;
            continue;
        }

        if (type == MSG_WRITE_ACK) {
            return LOOM_OK;
        }

        return LOOM_ERR_PROTOCOL;
    }
}

static int socket_poll_irq(loom_transport_t *t, uint32_t *irq_mask, int timeout_ms) {
    socket_priv_t *priv = (socket_priv_t *)t->priv;
    if (priv->fd < 0) return LOOM_ERR_NOT_CONNECTED;

    // First, return any accumulated IRQs
    if (priv->pending_irq) {
        *irq_mask = priv->pending_irq;
        priv->pending_irq = 0;
        return LOOM_OK;
    }

    // Poll for new messages
    struct pollfd pfd = { .fd = priv->fd, .events = POLLIN };
    int poll_ret = poll(&pfd, 1, timeout_ms);

    if (poll_ret < 0) {
        if (errno == EINTR) {
            *irq_mask = 0;
            return LOOM_OK;
        }
        return LOOM_ERR_TRANSPORT;
    }

    if (poll_ret == 0) {
        *irq_mask = 0;
        return LOOM_ERR_TIMEOUT;
    }

    // Read the message
    uint8_t type;
    uint32_t rdata, irq_bits;
    int rc = recv_message(priv->fd, &type, &rdata, &irq_bits);
    if (rc != LOOM_OK) return rc;

    if (type == MSG_IRQ) {
        *irq_mask = irq_bits;
        return LOOM_OK;
    }

    // Unexpected message - this shouldn't happen in poll context
    return LOOM_ERR_PROTOCOL;
}

// ============================================================================
// Transport vtable
// ============================================================================

static const loom_transport_ops_t socket_ops = {
    .connect = socket_connect,
    .disconnect = socket_disconnect,
    .read32 = socket_read32,
    .write32 = socket_write32,
    .poll_irq = socket_poll_irq,
};

// ============================================================================
// Public factory functions
// ============================================================================

loom_transport_t *loom_transport_socket_create(void) {
    loom_transport_t *t = (loom_transport_t *)calloc(1, sizeof(loom_transport_t));
    if (!t) return NULL;

    socket_priv_t *priv = (socket_priv_t *)calloc(1, sizeof(socket_priv_t));
    if (!priv) {
        free(t);
        return NULL;
    }

    priv->fd = -1;
    priv->pending_irq = 0;

    t->ops = &socket_ops;
    t->priv = priv;

    return t;
}

void loom_transport_socket_destroy(loom_transport_t *t) {
    if (t) {
        socket_priv_t *priv = (socket_priv_t *)t->priv;
        if (priv) {
            if (priv->fd >= 0) {
                close(priv->fd);
            }
            free(priv);
        }
        free(t);
    }
}
