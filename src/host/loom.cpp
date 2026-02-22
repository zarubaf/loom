// SPDX-License-Identifier: Apache-2.0
// Loom - Modern C++ Host Library Implementation

#include "loom.h"
#include "loom_log.h"
#include <unistd.h>

namespace loom {

static Logger logger = make_logger("loom");

// ============================================================================
// Context Implementation
// ============================================================================

Context::Context(std::unique_ptr<Transport> transport)
    : transport_(std::move(transport)) {}

Context::~Context() {
    disconnect();
}

Result<void> Context::connect(std::string_view target) {
    if (!transport_) {
        return Error::InvalidArg;
    }

    auto result = transport_->connect(target);
    if (!result.ok()) {
        return result.error();
    }

    // Read design info
    auto val = read32(addr::EmuCtrl + reg::NDpiFuncs);
    if (!val.ok()) return val.error();
    n_dpi_funcs_ = val.value();

    val = read32(addr::EmuCtrl + reg::MaxDpiArgs);
    if (!val.ok()) return val.error();
    max_dpi_args_ = val.value();
    if (max_dpi_args_ == 0) max_dpi_args_ = 8;  // fallback for older designs

    val = read32(addr::EmuCtrl + reg::TotalScanBits);
    if (!val.ok()) return val.error();
    scan_chain_length_ = val.value();

    val = read32(addr::EmuCtrl + reg::ShellVersion);
    if (!val.ok()) return val.error();
    shell_version_ = val.value();

    val = read32(addr::EmuCtrl + reg::NMemories);
    if (!val.ok()) return val.error();
    n_memories_ = val.value();

    // Read 8-word design hash
    for (int i = 0; i < 8; i++) {
        val = read32(addr::EmuCtrl + reg::DesignHash0 + i * 4);
        if (!val.ok()) return val.error();
        design_hash_[i] = val.value();
    }

    logger.info("Connected. Shell: %s, Hash: %.16s..., DPI funcs: %u, Scan bits: %u, Memories: %u",
             version_string(shell_version_).c_str(),
             design_hash_hex().c_str(),
             n_dpi_funcs_, scan_chain_length_, n_memories_);

    // Ensure emu_top is coupled (accessible through decoupler)
    auto couple_rc = couple();
    if (!couple_rc.ok()) {
        logger.warning("Failed to couple decoupler (may not be present)");
    }

    return {};
}

void Context::disconnect() {
    if (transport_) {
        transport_->disconnect();
    }
}

bool Context::is_connected() const {
    return transport_ && transport_->is_connected();
}

std::string Context::design_hash_hex() const {
    static const char hex[] = "0123456789abcdef";
    // Reconstruct big-endian hash from words (word 7 = MSB, word 0 = LSB)
    // Word 7 goes first in the hex string (most significant)
    std::string result(64, '0');
    for (int i = 7; i >= 0; i--) {
        int pos = (7 - i) * 8;  // word 7 → pos 0, word 0 → pos 56
        uint32_t w = design_hash_[i];
        // Big-endian byte order within each word
        for (int b = 3; b >= 0; b--) {
            uint8_t byte = (w >> (b * 8)) & 0xFF;
            result[pos++] = hex[(byte >> 4) & 0xF];
            result[pos++] = hex[byte & 0xF];
        }
    }
    return result;
}

// ============================================================================
// Low-level Register Access
// ============================================================================

Result<uint32_t> Context::read32(uint32_t addr) {
    if (!transport_) {
        return Error::InvalidArg;
    }
    return transport_->read32(addr);
}

Result<void> Context::write32(uint32_t addr, uint32_t data) {
    if (!transport_) {
        return Error::InvalidArg;
    }
    return transport_->write32(addr, data);
}

// ============================================================================
// Interrupt Support
// ============================================================================

Result<uint32_t> Context::wait_irq() {
    if (!transport_) {
        return Error::InvalidArg;
    }
    return transport_->wait_irq();
}

bool Context::has_irq_support() const {
    return transport_ && transport_->has_irq_support();
}

// ============================================================================
// Emulation Control
// ============================================================================

Result<State> Context::get_state() {
    auto val = read32(addr::EmuCtrl + reg::Status);
    if (!val.ok()) return val.error();
    return static_cast<State>(val.value() & 0x7);
}

Result<void> Context::start() {
    return write32(addr::EmuCtrl + reg::Control, cmd::Start);
}

Result<void> Context::stop() {
    return write32(addr::EmuCtrl + reg::Control, cmd::Stop);
}

Result<void> Context::step(uint32_t n_cycles) {
    // SW-based stepping: set time_cmp = current_time + N, then start
    auto time = get_time();
    if (!time.ok()) return time.error();

    auto rc = set_time_compare(time.value() + n_cycles);
    if (!rc.ok()) return rc;

    return write32(addr::EmuCtrl + reg::Control, cmd::Start);
}

Result<void> Context::reset() {
    return write32(addr::EmuCtrl + reg::Control, cmd::Reset);
}

Result<uint64_t> Context::get_cycle_count() {
    auto lo = read32(addr::EmuCtrl + reg::CycleLo);
    if (!lo.ok()) return lo.error();

    auto hi = read32(addr::EmuCtrl + reg::CycleHi);
    if (!hi.ok()) return hi.error();

    return (static_cast<uint64_t>(hi.value()) << 32) | lo.value();
}

Result<void> Context::finish(int exit_code) {
    uint32_t val = 0x01 | ((exit_code & 0xFF) << 8);
    return write32(addr::EmuCtrl + reg::Finish, val);
}

Result<uint64_t> Context::get_time() {
    auto lo = read32(addr::EmuCtrl + reg::TimeLo);
    if (!lo.ok()) return lo.error();

    auto hi = read32(addr::EmuCtrl + reg::TimeHi);
    if (!hi.ok()) return hi.error();

    return (static_cast<uint64_t>(hi.value()) << 32) | lo.value();
}

Result<void> Context::set_time_compare(uint64_t value) {
    auto rc = write32(addr::EmuCtrl + reg::TimeCmpLo,
                      static_cast<uint32_t>(value & 0xFFFFFFFF));
    if (!rc.ok()) return rc;

    return write32(addr::EmuCtrl + reg::TimeCmpHi,
                   static_cast<uint32_t>(value >> 32));
}

Result<uint64_t> Context::get_time_compare() {
    auto lo = read32(addr::EmuCtrl + reg::TimeCmpLo);
    if (!lo.ok()) return lo.error();

    auto hi = read32(addr::EmuCtrl + reg::TimeCmpHi);
    if (!hi.ok()) return hi.error();

    return (static_cast<uint64_t>(hi.value()) << 32) | lo.value();
}

// ============================================================================
// DPI Function Handling
// ============================================================================

static inline uint32_t dpi_func_addr(uint32_t func_id, uint32_t reg_offset) {
    return addr::DpiRegfile + (func_id * reg::DpiFuncSize) + reg_offset;
}

Result<uint32_t> Context::dpi_poll() {
    return read32(addr::DpiRegfile + reg::DpiPendingMask);
}

Result<DpiCall> Context::dpi_get_call(uint32_t func_id) {
    if (func_id >= n_dpi_funcs_) {
        return Error::InvalidArg;
    }

    DpiCall call;
    call.func_id = func_id;
    call.args.resize(max_dpi_args_);

    // Read all arguments
    for (uint32_t i = 0; i < max_dpi_args_; i++) {
        auto arg = read32(dpi_func_addr(func_id, reg::DpiArg0 + i * 4));
        if (!arg.ok()) return arg.error();
        call.args[i] = arg.value();
    }

    return call;
}

Result<void> Context::dpi_complete(uint32_t func_id, uint64_t result) {
    if (func_id >= n_dpi_funcs_) {
        return Error::InvalidArg;
    }

    uint32_t result_lo_offset = reg::DpiArg0 + max_dpi_args_ * 4;
    auto rc = write32(dpi_func_addr(func_id, result_lo_offset),
                      static_cast<uint32_t>(result & 0xFFFFFFFF));
    if (!rc.ok()) return rc;

    rc = write32(dpi_func_addr(func_id, result_lo_offset + 4),
                 static_cast<uint32_t>(result >> 32));
    if (!rc.ok()) return rc;

    return write32(dpi_func_addr(func_id, reg::DpiControl), ctrl::DpiSetDone);
}

Result<void> Context::dpi_write_arg(uint32_t func_id, int arg_idx, uint32_t value) {
    if (func_id >= n_dpi_funcs_ || arg_idx < 0 || static_cast<uint32_t>(arg_idx) >= max_dpi_args_) {
        return Error::InvalidArg;
    }
    return write32(dpi_func_addr(func_id, reg::DpiArg0 + arg_idx * 4), value);
}

Result<void> Context::dpi_error(uint32_t func_id) {
    if (func_id >= n_dpi_funcs_) {
        return Error::InvalidArg;
    }

    return write32(dpi_func_addr(func_id, reg::DpiControl),
                   ctrl::DpiSetDone | ctrl::DpiSetError);
}

// ============================================================================
// Scan Chain Control
// ============================================================================

Result<void> Context::scan_clear_done() {
    return write32(addr::ScanCtrl + reg::ScanStatus, status::ScanDone);
}

Result<void> Context::scan_wait_done(int timeout_ms) {
    int elapsed = 0;
    const int poll_interval = 10;  // ms

    while (elapsed < timeout_ms) {
        auto status_result = read32(addr::ScanCtrl + reg::ScanStatus);
        if (!status_result.ok()) return status_result.error();

        if (status_result.value() & status::ScanDone) {
            return {};
        }

        usleep(poll_interval * 1000);  // Convert ms to microseconds
        elapsed += poll_interval;
    }

    return Error::Timeout;
}

Result<void> Context::scan_capture(int timeout_ms) {
    auto rc = scan_clear_done();
    if (!rc.ok()) return rc;

    rc = write32(addr::ScanCtrl + reg::ScanControl, cmd::ScanCapture);
    if (!rc.ok()) return rc;

    return scan_wait_done(timeout_ms);
}

Result<void> Context::scan_restore(int timeout_ms) {
    auto rc = scan_clear_done();
    if (!rc.ok()) return rc;

    rc = write32(addr::ScanCtrl + reg::ScanControl, cmd::ScanRestore);
    if (!rc.ok()) return rc;

    return scan_wait_done(timeout_ms);
}

Result<std::vector<uint32_t>> Context::scan_read_data() {
    uint32_t n_words = (scan_chain_length_ + 31) / 32;
    std::vector<uint32_t> data(n_words);

    for (uint32_t i = 0; i < n_words; i++) {
        auto val = read32(addr::ScanCtrl + reg::ScanDataBase + (i * 4));
        if (!val.ok()) return val.error();
        data[i] = val.value();
    }

    return data;
}

Result<void> Context::scan_write_data(const std::vector<uint32_t>& data) {
    for (size_t i = 0; i < data.size(); i++) {
        auto rc = write32(addr::ScanCtrl + reg::ScanDataBase + (i * 4), data[i]);
        if (!rc.ok()) return rc;
    }
    return {};
}

Result<bool> Context::scan_is_busy() {
    auto status_result = read32(addr::ScanCtrl + reg::ScanStatus);
    if (!status_result.ok()) return status_result.error();
    return (status_result.value() & status::ScanBusy) != 0;
}

// ============================================================================
// Memory Shadow Access
// ============================================================================

Result<void> Context::mem_clear_done() {
    return write32(addr::MemCtrl + reg::MemStatus, status::MemDone);
}

Result<void> Context::mem_wait_done(int timeout_ms) {
    int elapsed = 0;
    const int poll_interval = 10;  // ms

    while (elapsed < timeout_ms) {
        auto status_result = read32(addr::MemCtrl + reg::MemStatus);
        if (!status_result.ok()) return status_result.error();

        if (status_result.value() & status::MemDone) {
            return {};
        }

        usleep(poll_interval * 1000);
        elapsed += poll_interval;
    }

    return Error::Timeout;
}

Result<void> Context::mem_write_entry(uint32_t global_addr, const std::vector<uint32_t>& data) {
    // Write data words
    for (size_t i = 0; i < data.size(); i++) {
        auto rc = write32(addr::MemCtrl + reg::MemDataBase + (i * 4), data[i]);
        if (!rc.ok()) return rc;
    }
    // Write address
    auto rc = write32(addr::MemCtrl + reg::MemAddr, global_addr);
    if (!rc.ok()) return rc;

    // Issue write command
    rc = mem_clear_done();
    if (!rc.ok()) return rc;
    rc = write32(addr::MemCtrl + reg::MemControl, cmd::MemWrite);
    if (!rc.ok()) return rc;

    return mem_wait_done(1000);
}

Result<std::vector<uint32_t>> Context::mem_read_entry(uint32_t global_addr, int n_data_words) {
    // Write address
    auto rc = write32(addr::MemCtrl + reg::MemAddr, global_addr);
    if (!rc.ok()) return rc.error();

    // Issue read command
    rc = mem_clear_done();
    if (!rc.ok()) return rc.error();
    rc = write32(addr::MemCtrl + reg::MemControl, cmd::MemRead);
    if (!rc.ok()) return rc.error();

    rc = mem_wait_done(1000);
    if (!rc.ok()) return rc.error();

    // Read data words
    std::vector<uint32_t> data(n_data_words);
    for (int i = 0; i < n_data_words; i++) {
        auto val = read32(addr::MemCtrl + reg::MemDataBase + (i * 4));
        if (!val.ok()) return val.error();
        data[i] = val.value();
    }

    return data;
}

Result<void> Context::mem_preload_start(uint32_t global_addr, const std::vector<uint32_t>& data) {
    // Write data words
    for (size_t i = 0; i < data.size(); i++) {
        auto rc = write32(addr::MemCtrl + reg::MemDataBase + (i * 4), data[i]);
        if (!rc.ok()) return rc;
    }
    // Write start address
    auto rc = write32(addr::MemCtrl + reg::MemAddr, global_addr);
    if (!rc.ok()) return rc;

    // Issue preload start command
    rc = mem_clear_done();
    if (!rc.ok()) return rc;
    rc = write32(addr::MemCtrl + reg::MemControl, cmd::MemPreloadStart);
    if (!rc.ok()) return rc;

    return mem_wait_done(1000);
}

Result<void> Context::mem_preload_next(const std::vector<uint32_t>& data) {
    // Write data words
    for (size_t i = 0; i < data.size(); i++) {
        auto rc = write32(addr::MemCtrl + reg::MemDataBase + (i * 4), data[i]);
        if (!rc.ok()) return rc;
    }

    // Issue preload next command (auto-increments address)
    auto rc = mem_clear_done();
    if (!rc.ok()) return rc;
    rc = write32(addr::MemCtrl + reg::MemControl, cmd::MemPreloadNext);
    if (!rc.ok()) return rc;

    return mem_wait_done(1000);
}

// ============================================================================
// Decoupler Control
// ============================================================================

Result<void> Context::couple() {
    // Read-modify-write: clear bit 2 (decouple), preserve bit 0 (lockdown)
    auto val = read32(addr::Firewall + reg::FwCtrl);
    if (!val.ok()) return val.error();
    uint32_t ctrl = val.value() & ~0x4u;  // clear decouple bit
    return write32(addr::Firewall + reg::FwCtrl, ctrl);
}

Result<void> Context::decouple() {
    // Read-modify-write: set bit 2 (decouple), preserve bit 0 (lockdown)
    auto val = read32(addr::Firewall + reg::FwCtrl);
    if (!val.ok()) return val.error();
    uint32_t ctrl = val.value() | 0x4u;  // set decouple bit
    return write32(addr::Firewall + reg::FwCtrl, ctrl);
}

Result<bool> Context::is_coupled() {
    auto val = read32(addr::Firewall + reg::FwStatus);
    if (!val.ok()) return val.error();
    return (val.value() & 0x8) == 0;  // bit 3 = decouple_status
}

} // namespace loom
