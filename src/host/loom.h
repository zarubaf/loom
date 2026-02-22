// SPDX-License-Identifier: Apache-2.0
// Loom - Modern C++ Host Library for FPGA Emulation Control
//
// This library provides a transport-agnostic interface for controlling
// Loom-instrumented designs. It supports both socket (simulation) and
// PCIe (FPGA) transports.

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <stdexcept>

namespace loom {

// ============================================================================
// Error Handling
// ============================================================================

enum class Error {
    Ok = 0,
    Transport = -1,
    Timeout = -2,
    InvalidArg = -3,
    NotConnected = -4,
    Protocol = -5,
    DpiError = -6,
    Shutdown = -7,
    Interrupted = -8,    // Signal received during blocking wait (EINTR)
    NotSupported = -9,   // Operation not supported by this transport
};

// Exception for loom errors
class Exception : public std::runtime_error {
public:
    explicit Exception(Error code, std::string_view message = "")
        : std::runtime_error(std::string(message)), code_(code) {}

    Error code() const { return code_; }

private:
    Error code_;
};

// Result type for operations that can fail
template<typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)), error_(Error::Ok) {}
    Result(Error error) : error_(error) {}

    bool ok() const { return error_ == Error::Ok; }
    Error error() const { return error_; }

    T& value() { return value_; }
    const T& value() const { return value_; }

    T value_or(T default_value) const {
        return ok() ? value_ : default_value;
    }

private:
    T value_{};
    Error error_;
};

// Specialization for void
template<>
class Result<void> {
public:
    Result() : error_(Error::Ok) {}
    Result(Error error) : error_(error) {}

    bool ok() const { return error_ == Error::Ok; }
    Error error() const { return error_; }

private:
    Error error_;
};

// ============================================================================
// Emulation States
// ============================================================================

enum class State : uint8_t {
    Idle = 0,
    Running = 1,
    Frozen = 2,
    Snapshot = 3,
    Restore = 4,
    Error = 5,
};

// ============================================================================
// Address Map Constants
// ============================================================================

namespace addr {
    constexpr uint32_t EmuCtrl = 0x00000;
    constexpr uint32_t DpiRegfile = 0x10000;
    constexpr uint32_t ScanCtrl = 0x20000;
    constexpr uint32_t MemCtrl = 0x30000;
    constexpr uint32_t ClkGen = 0x40000;
    constexpr uint32_t ShellCtrl = 0x50000;
}

namespace reg {
    // emu_ctrl register offsets (compacted layout)
    constexpr uint32_t Status = 0x00;
    constexpr uint32_t Control = 0x04;
    constexpr uint32_t CycleLo = 0x08;
    constexpr uint32_t CycleHi = 0x0C;
    constexpr uint32_t ClkDiv = 0x10;
    constexpr uint32_t NDpiFuncs = 0x14;
    constexpr uint32_t NMemories = 0x18;
    constexpr uint32_t NScanChains = 0x1C;
    constexpr uint32_t TotalScanBits = 0x20;
    constexpr uint32_t MaxDpiArgs = 0x24;
    constexpr uint32_t ShellVersion = 0x28;
    constexpr uint32_t IrqStatus = 0x2C;
    constexpr uint32_t IrqEnable = 0x30;
    constexpr uint32_t Finish = 0x34;
    constexpr uint32_t TimeLo = 0x38;
    constexpr uint32_t TimeHi = 0x3C;
    constexpr uint32_t TimeCmpLo = 0x40;
    constexpr uint32_t TimeCmpHi = 0x44;
    constexpr uint32_t DesignHash0 = 0x48;  // 8 contiguous words
    constexpr uint32_t DesignHash1 = 0x4C;
    constexpr uint32_t DesignHash2 = 0x50;
    constexpr uint32_t DesignHash3 = 0x54;
    constexpr uint32_t DesignHash4 = 0x58;
    constexpr uint32_t DesignHash5 = 0x5C;
    constexpr uint32_t DesignHash6 = 0x60;
    constexpr uint32_t DesignHash7 = 0x64;

    // DPI regfile register offsets (per function, 64 bytes each)
    constexpr uint32_t DpiFuncSize = 0x40;
    constexpr uint32_t DpiStatus = 0x00;
    constexpr uint32_t DpiControl = 0x04;
    constexpr uint32_t DpiArg0 = 0x08;
    // DpiResultLo/Hi follow after MAX_ARGS arg registers:
    //   DpiResultLo = DpiArg0 + max_args * 4
    //   DpiResultHi = DpiResultLo + 4

    // Global DPI pending mask (func_idx=1023, one bit per function)
    constexpr uint32_t DpiPendingMask = 0xFFC0;

    // shell control register offsets
    constexpr uint32_t DecouplerCtrl = 0x00;

    // scan_ctrl register offsets
    constexpr uint32_t ScanStatus = 0x00;
    constexpr uint32_t ScanControl = 0x04;
    constexpr uint32_t ScanLength = 0x08;
    constexpr uint32_t ScanDataBase = 0x10;

    // mem_ctrl register offsets
    constexpr uint32_t MemStatus = 0x00;
    constexpr uint32_t MemControl = 0x04;
    constexpr uint32_t MemAddr = 0x08;
    constexpr uint32_t MemLength = 0x0C;
    constexpr uint32_t MemDataBase = 0x10;
}

namespace cmd {
    constexpr uint32_t Start = 0x01;
    constexpr uint32_t Stop = 0x02;
    constexpr uint32_t Reset = 0x03;
    constexpr uint32_t Snapshot = 0x04;
    constexpr uint32_t Restore = 0x05;

    constexpr uint32_t ScanCapture = 0x01;
    constexpr uint32_t ScanRestore = 0x02;

    constexpr uint32_t MemRead = 0x01;
    constexpr uint32_t MemWrite = 0x02;
    constexpr uint32_t MemPreloadStart = 0x03;
    constexpr uint32_t MemPreloadNext = 0x04;
}

namespace status {
    constexpr uint32_t DpiPending = 1 << 0;
    constexpr uint32_t DpiDone = 1 << 1;
    constexpr uint32_t DpiError = 1 << 2;

    constexpr uint32_t ScanBusy = 1 << 0;
    constexpr uint32_t ScanDone = 1 << 1;

    constexpr uint32_t MemBusy = 1 << 0;
    constexpr uint32_t MemDone = 1 << 1;
}

namespace ctrl {
    constexpr uint32_t DpiAck = 1 << 0;
    constexpr uint32_t DpiSetDone = 1 << 1;
    constexpr uint32_t DpiSetError = 1 << 2;
}

// ============================================================================
// Shell Version
// ============================================================================

constexpr uint32_t LOOM_SHELL_VERSION = 0x000100;  // 0.1.0

// Convert packed version (0xMMNNPP) to "M.N.P" string
inline std::string version_string(uint32_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u",
                  (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return buf;
}

// ============================================================================
// Transport Interface
// ============================================================================

class Transport {
public:
    virtual ~Transport() = default;

    virtual Result<void> connect(std::string_view target) = 0;
    virtual void disconnect() = 0;
    virtual Result<uint32_t> read32(uint32_t addr) = 0;
    virtual Result<void> write32(uint32_t addr, uint32_t data) = 0;

    // Block until a hardware interrupt fires. Returns IRQ bitmask.
    //
    // Socket:  blocks on recv() waiting for type=2 (IRQ) or type=3 (shutdown)
    // XDMA:    blocks on read(events_fd) waiting for MSI
    //
    // Returns:
    //   Ok(bitmask)        — interrupt fired, bitmask indicates which IRQ lines
    //   Error::Shutdown    — emulation ended (shutdown message or EOF)
    //   Error::Interrupted — signal received (EINTR), caller should check flags
    //   Error::NotSupported — transport has no interrupt capability (use polling)
    virtual Result<uint32_t> wait_irq() = 0;

    // Returns true if the transport supports interrupt-driven wait_irq()
    virtual bool has_irq_support() const = 0;

    virtual bool is_connected() const = 0;
};

// ============================================================================
// DPI Call Information
// ============================================================================

struct DpiCall {
    uint32_t func_id;
    std::vector<uint32_t> args;
};

// ============================================================================
// Loom Context
// ============================================================================

class Context {
public:
    explicit Context(std::unique_ptr<Transport> transport);
    ~Context();

    // Non-copyable
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    // Movable
    Context(Context&&) = default;
    Context& operator=(Context&&) = default;

    // Connection
    Result<void> connect(std::string_view target);
    void disconnect();
    bool is_connected() const;

    // Design info accessors
    uint32_t n_dpi_funcs() const { return n_dpi_funcs_; }
    uint32_t max_dpi_args() const { return max_dpi_args_; }
    uint32_t scan_chain_length() const { return scan_chain_length_; }
    uint32_t shell_version() const { return shell_version_; }
    const std::array<uint32_t, 8>& design_hash() const { return design_hash_; }

    // Return design hash as hex string (64 chars)
    std::string design_hash_hex() const;

    // ========================================================================
    // Emulation Control
    // ========================================================================

    Result<State> get_state();
    Result<void> start();
    Result<void> stop();
    Result<void> step(uint32_t n_cycles);
    Result<void> reset();
    Result<uint64_t> get_cycle_count();
    Result<void> finish(int exit_code);

    Result<uint64_t> get_time();
    Result<void> set_time_compare(uint64_t value);
    Result<uint64_t> get_time_compare();

    // ========================================================================
    // DPI Function Handling
    // ========================================================================

    Result<uint32_t> dpi_poll();  // Returns pending mask
    Result<DpiCall> dpi_get_call(uint32_t func_id);
    Result<void> dpi_complete(uint32_t func_id, uint64_t result);
    Result<void> dpi_write_arg(uint32_t func_id, int arg_idx, uint32_t value);
    Result<void> dpi_error(uint32_t func_id);

    // ========================================================================
    // Memory Shadow Access
    // ========================================================================

    uint32_t n_memories() const { return n_memories_; }
    Result<void> mem_write_entry(uint32_t global_addr, const std::vector<uint32_t>& data);
    Result<std::vector<uint32_t>> mem_read_entry(uint32_t global_addr, int n_data_words);
    Result<void> mem_preload_start(uint32_t global_addr, const std::vector<uint32_t>& data);
    Result<void> mem_preload_next(const std::vector<uint32_t>& data);

    // ========================================================================
    // Scan Chain Control
    // ========================================================================

    Result<void> scan_capture(int timeout_ms = 5000);
    Result<void> scan_restore(int timeout_ms = 5000);
    Result<std::vector<uint32_t>> scan_read_data();
    Result<void> scan_write_data(const std::vector<uint32_t>& data);
    Result<bool> scan_is_busy();
    Result<void> scan_clear_done();

    // ========================================================================
    // Decoupler Control
    // ========================================================================

    Result<void> couple();
    Result<void> decouple();
    Result<bool> is_coupled();

    // ========================================================================
    // Interrupt Support
    // ========================================================================

    Result<uint32_t> wait_irq();
    bool has_irq_support() const;

    // ========================================================================
    // Low-level Register Access
    // ========================================================================

    Result<uint32_t> read32(uint32_t addr);
    Result<void> write32(uint32_t addr, uint32_t data);

private:
    Result<void> scan_wait_done(int timeout_ms);
    Result<void> mem_wait_done(int timeout_ms);
    Result<void> mem_clear_done();

    std::unique_ptr<Transport> transport_;
    uint32_t n_dpi_funcs_ = 0;
    uint32_t max_dpi_args_ = 8;
    uint32_t scan_chain_length_ = 0;
    uint32_t n_memories_ = 0;
    uint32_t shell_version_ = 0;
    std::array<uint32_t, 8> design_hash_ = {};
};

// ============================================================================
// Transport Factory Functions
// ============================================================================

std::unique_ptr<Transport> create_socket_transport();
std::unique_ptr<Transport> create_xdma_transport();

} // namespace loom
