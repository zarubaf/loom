// SPDX-License-Identifier: Apache-2.0
// Loom XDMA Transport Implementation
//
// Supports two modes:
//   1. /dev/xdma0_user — via Xilinx XDMA kernel driver (pread/pwrite)
//   2. sysfs resource   — direct BAR0 mmap (no driver needed)
//
// The target string selects the mode:
//   /dev/xdma*       → uses pread/pwrite
//   /sys/bus/pci/...  → mmap the resource file
//   0000:17:00.0      → auto-constructs sysfs resource0 path

#include "loom.h"
#include "loom_log.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cerrno>
#include <cstring>
#include <string>

namespace loom {

static Logger logger = make_logger("xdma");

class XdmaTransport : public Transport {
public:
    XdmaTransport() = default;
    ~XdmaTransport() override { disconnect(); }

    XdmaTransport(const XdmaTransport&) = delete;
    XdmaTransport& operator=(const XdmaTransport&) = delete;

    Result<void> connect(std::string_view target) override;
    void disconnect() override;
    Result<uint32_t> read32(uint32_t addr) override;
    Result<void> write32(uint32_t addr, uint32_t data) override;
    Result<uint32_t> wait_irq() override;
    bool has_irq_support() const override { return events_fd_ >= 0; }
    bool is_connected() const override { return mmapped_ ? (bar_ != nullptr) : (fd_ >= 0); }

private:
    int fd_ = -1;
    int events_fd_ = -1;  // /dev/xdma0_events_0 for MSI support
    volatile uint32_t *bar_ = nullptr;
    size_t bar_size_ = 0;
    bool mmapped_ = false;
};

Result<void> XdmaTransport::connect(std::string_view target) {
    if (is_connected()) return {};

    std::string path(target);

    // If target looks like a PCI BDF (e.g. "0000:17:00.0"), construct sysfs path
    if (path.size() >= 10 && path[4] == ':' && path[7] == ':') {
        path = "/sys/bus/pci/devices/" + path + "/resource0";
    }

    // Decide mode based on path
    if (path.find("/sys/") == 0 || path.find("resource") != std::string::npos) {
        // mmap mode — direct BAR access
        fd_ = ::open(path.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            logger.error("open('%s') failed: %s", path.c_str(), strerror(errno));
            return Error::Transport;
        }

        // Get file size for BAR length
        off_t size = ::lseek(fd_, 0, SEEK_END);
        if (size <= 0) {
            // Default to 1MB BAR
            size = 1 << 20;
        }
        bar_size_ = static_cast<size_t>(size);

        bar_ = static_cast<volatile uint32_t *>(
            ::mmap(nullptr, bar_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        if (bar_ == MAP_FAILED) {
            logger.error("mmap('%s', %zu) failed: %s", path.c_str(), bar_size_, strerror(errno));
            ::close(fd_);
            fd_ = -1;
            bar_ = nullptr;
            return Error::Transport;
        }

        mmapped_ = true;
        logger.info("Connected to %s (mmap, %zu bytes)", path.c_str(), bar_size_);
    } else {
        // pread/pwrite mode — XDMA driver char device
        fd_ = ::open(path.c_str(), O_RDWR | O_SYNC);
        if (fd_ < 0) {
            logger.error("open('%s') failed: %s", path.c_str(), strerror(errno));
            return Error::Transport;
        }

        mmapped_ = false;
        logger.info("Connected to %s (pread/pwrite)", path.c_str());

        // Try to open events device for interrupt support.
        // Derive path: /dev/xdma0_user → /dev/xdma0_events_0
        auto user_pos = path.find("_user");
        if (user_pos != std::string::npos) {
            std::string events_path = path.substr(0, user_pos) + "_events_0";
            events_fd_ = ::open(events_path.c_str(), O_RDONLY);
            if (events_fd_ >= 0) {
                logger.info("Opened %s for interrupt support", events_path.c_str());
            }
        }
    }

    return {};
}

void XdmaTransport::disconnect() {
    if (events_fd_ >= 0) {
        ::close(events_fd_);
        events_fd_ = -1;
    }
    if (bar_ && bar_ != MAP_FAILED) {
        ::munmap(const_cast<uint32_t *>(bar_), bar_size_);
        bar_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    mmapped_ = false;
}

Result<uint32_t> XdmaTransport::read32(uint32_t addr) {
    if (!is_connected()) return Error::NotConnected;

    if (mmapped_) {
        if (addr + 4 > bar_size_) {
            logger.error("read32(0x%05x) out of range (bar_size=0x%zx)", addr, bar_size_);
            return Error::InvalidArg;
        }
        uint32_t val = bar_[addr / 4];
        return val;
    } else {
        uint32_t val = 0;
        ssize_t n = ::pread(fd_, &val, 4, addr);
        if (n != 4) {
            logger.error("pread(addr=0x%05x) failed: %s", addr, strerror(errno));
            return Error::Transport;
        }
        return val;
    }
}

Result<void> XdmaTransport::write32(uint32_t addr, uint32_t data) {
    if (!is_connected()) return Error::NotConnected;

    if (mmapped_) {
        if (addr + 4 > bar_size_) {
            logger.error("write32(0x%05x) out of range (bar_size=0x%zx)", addr, bar_size_);
            return Error::InvalidArg;
        }
        bar_[addr / 4] = data;
        return {};
    } else {
        ssize_t n = ::pwrite(fd_, &data, 4, addr);
        if (n != 4) {
            logger.error("pwrite(addr=0x%05x, data=0x%08x) failed: %s",
                         addr, data, strerror(errno));
            return Error::Transport;
        }
        return {};
    }
}

Result<uint32_t> XdmaTransport::wait_irq() {
    if (events_fd_ < 0) {
        return Error::NotSupported;
    }

    // Block until MSI fires (matches kernel driver's wait_event_interruptible).
    // The XDMA driver's events device: read() blocks until an interrupt fires,
    // then returns the event count and auto-acknowledges.
    uint32_t events = 0;
    ssize_t n = ::read(events_fd_, &events, sizeof(events));
    if (n < 0) {
        if (errno == EINTR) return Error::Interrupted;
        logger.error("events read failed: %s", strerror(errno));
        return Error::Transport;
    }
    if (n != sizeof(events)) {
        logger.error("events read: short read (%zd bytes)", n);
        return Error::Transport;
    }

    return events;
}

std::unique_ptr<Transport> create_xdma_transport() {
    return std::make_unique<XdmaTransport>();
}

} // namespace loom
