// SPDX-License-Identifier: Apache-2.0
// Loom Socket Transport Implementation
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
//   [0]     : type (0=read response, 1=write ack, 2=irq, 3=shutdown)
//   [1-3]   : reserved
//   [4-7]   : read data (little-endian)
//   [8-11]  : irq bits (little-endian)

#include "loom.h"
#include "loom_log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

namespace loom {

static Logger logger = make_logger("socket");

// Message types
namespace msg {
    constexpr uint8_t Read = 0;
    constexpr uint8_t Write = 1;
    constexpr uint8_t ReadResp = 0;
    constexpr uint8_t WriteAck = 1;
    constexpr uint8_t Irq = 2;
    constexpr uint8_t Shutdown = 3;
}

// ============================================================================
// Socket Transport Implementation
// ============================================================================

class SocketTransport : public Transport {
public:
    SocketTransport() = default;
    ~SocketTransport() override { disconnect(); }

    // Non-copyable
    SocketTransport(const SocketTransport&) = delete;
    SocketTransport& operator=(const SocketTransport&) = delete;

    Result<void> connect(std::string_view target) override;
    void disconnect() override;
    Result<uint32_t> read32(uint32_t addr) override;
    Result<void> write32(uint32_t addr, uint32_t data) override;
    Result<uint32_t> poll_irq(int timeout_ms) override;
    bool is_connected() const override { return fd_ >= 0; }

private:
    Result<void> send_message(uint8_t type, uint32_t addr, uint32_t wdata);
    Result<std::tuple<uint8_t, uint32_t, uint32_t>> recv_message();

    int fd_ = -1;
    uint32_t pending_irq_ = 0;
};

// ============================================================================
// Helper Methods
// ============================================================================

Result<void> SocketTransport::send_message(uint8_t type, uint32_t addr, uint32_t wdata) {
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
        ssize_t n = ::write(fd_, buf + total, 12 - total);
        if (n <= 0) {
            if (errno == EINTR) continue;
            logger.error("Write failed: %s", strerror(errno));
            return Error::Transport;
        }
        total += static_cast<size_t>(n);
    }
    return {};
}

Result<std::tuple<uint8_t, uint32_t, uint32_t>> SocketTransport::recv_message() {
    uint8_t buf[12];
    size_t total = 0;
    while (total < 12) {
        ssize_t n = ::read(fd_, buf + total, 12 - total);
        if (n <= 0) {
            if (errno == EINTR) continue;
            logger.error("Read failed: %s", strerror(errno));
            return Error::Transport;
        }
        total += static_cast<size_t>(n);
    }

    uint8_t type = buf[0];
    uint32_t rdata = buf[4] | (buf[5] << 8) | (buf[6] << 16) | (buf[7] << 24);
    uint32_t irq_bits = buf[8] | (buf[9] << 8) | (buf[10] << 16) | (buf[11] << 24);

    return std::make_tuple(type, rdata, irq_bits);
}

// ============================================================================
// Transport Operations
// ============================================================================

Result<void> SocketTransport::connect(std::string_view target) {
    if (fd_ >= 0) {
        return {};  // Already connected
    }

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        logger.error("socket() failed: %s", strerror(errno));
        return Error::Transport;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    // Copy target to sun_path, ensuring null termination
    size_t max_len = sizeof(addr.sun_path) - 1;
    size_t copy_len = std::min(target.size(), max_len);
    std::memcpy(addr.sun_path, target.data(), copy_len);
    addr.sun_path[copy_len] = '\0';

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger.error("connect() to '%.*s' failed: %s",
                  static_cast<int>(target.size()), target.data(), strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return Error::Transport;
    }

    logger.info("Connected to %.*s", static_cast<int>(target.size()), target.data());
    return {};
}

void SocketTransport::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        logger.debug("Disconnected");
    }
}

Result<uint32_t> SocketTransport::read32(uint32_t addr) {
    if (fd_ < 0) return Error::NotConnected;

    auto rc = send_message(msg::Read, addr, 0);
    if (!rc.ok()) return rc.error();

    // Wait for response, handling any IRQ messages that arrive first
    while (true) {
        auto result = recv_message();
        if (!result.ok()) return result.error();

        auto [type, rdata, irq_bits] = result.value();

        if (type == msg::Irq) {
            pending_irq_ |= irq_bits;
            continue;
        }

        if (type == msg::Shutdown) {
            return Error::Shutdown;
        }

        if (type == msg::ReadResp) {
            return rdata;
        }

        logger.error("Unexpected message type %u in read32", type);
        return Error::Protocol;
    }
}

Result<void> SocketTransport::write32(uint32_t addr, uint32_t data) {
    if (fd_ < 0) return Error::NotConnected;

    auto rc = send_message(msg::Write, addr, data);
    if (!rc.ok()) return rc.error();

    // Wait for ack, handling any IRQ messages that arrive first
    while (true) {
        auto result = recv_message();
        if (!result.ok()) return result.error();

        auto [type, rdata, irq_bits] = result.value();

        if (type == msg::Irq) {
            pending_irq_ |= irq_bits;
            continue;
        }

        if (type == msg::Shutdown) {
            return Error::Shutdown;
        }

        if (type == msg::WriteAck) {
            return {};
        }

        logger.error("Unexpected message type %u in write32", type);
        return Error::Protocol;
    }
}

Result<uint32_t> SocketTransport::poll_irq(int timeout_ms) {
    if (fd_ < 0) return Error::NotConnected;

    // First, return any accumulated IRQs
    if (pending_irq_) {
        uint32_t irq = pending_irq_;
        pending_irq_ = 0;
        return irq;
    }

    // Poll for new messages
    struct pollfd pfd = { .fd = fd_, .events = POLLIN, .revents = 0 };
    int poll_ret = ::poll(&pfd, 1, timeout_ms);

    if (poll_ret < 0) {
        if (errno == EINTR) {
            return 0u;  // No IRQs
        }
        logger.error("poll() failed: %s", strerror(errno));
        return Error::Transport;
    }

    if (poll_ret == 0) {
        return Error::Timeout;
    }

    // Read the message
    auto result = recv_message();
    if (!result.ok()) return result.error();

    auto [type, rdata, irq_bits] = result.value();

    if (type == msg::Irq) {
        return irq_bits;
    }

    if (type == msg::Shutdown) {
        return Error::Shutdown;
    }

    logger.error("Unexpected message type %u in poll_irq", type);
    return Error::Protocol;
}

// ============================================================================
// Factory Function
// ============================================================================

std::unique_ptr<Transport> create_socket_transport() {
    return std::make_unique<SocketTransport>();
}

} // namespace loom
