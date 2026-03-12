// SPDX-License-Identifier: Apache-2.0
// Loom - Modern C++ Host Library Implementation

#include "loom.h"
#include "loom_log.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <vector>

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

Result<void> Context::connect(std::string_view target, uint32_t freq_mhz) {
    if (!transport_) {
        return Error::InvalidArg;
    }

    auto result = transport_->connect(target);
    if (!result.ok()) {
        return result.error();
    }

    // Probe firewall mgmt first — this is on aclk (no CDC), so it always
    // responds if the shell is alive.  Reading emu_ctrl registers goes
    // through the CDC which blocks if emu_clk isn't running.
    auto probe = read32(addr::Firewall + reg::FwTimeoutCycles);
    if (!probe.ok()) {
        logger.error("Cannot reach firewall management registers");
        return probe.error();
    }
    if (probe.value() == 0xFFFFFFFF) {
        logger.error("Shell not responding (firewall reads 0xFFFFFFFF). "
                     "Check PCIe link and FPGA programming.");
        return Error::Transport;
    }

    // Program emu_clk to target frequency before accessing emu_top.
    // ClkGen is on aclk (no CDC) — safe to access before couple().
    // On FPGA, emu_clk defaults to 50 MHz; must be reconfigured to the
    // design's target frequency before CDC-crossed reads will work.
    if (freq_mhz > 0) {
        auto clk_rc = configure_clock(freq_mhz);
        if (!clk_rc.ok()) {
            logger.error("Clock configuration failed");
            return clk_rc.error();
        }
    }

    // Ensure emu_top is coupled (accessible through decoupler/firewall)
    auto couple_rc = couple();
    if (!couple_rc.ok()) {
        logger.warning("Failed to couple decoupler (may not be present)");
    }

    // Now safe to read EM registers (through firewall → CDC → emu_top)
    auto rc = probe_rm();
    if (!rc.ok()) return rc;

    logger.info("Connected. Shell: %s, Hash: %.16s..., DPI funcs: %u, Scan bits: %u, Memories: %u, FIFO words: %u",
             version_string(shell_version_).c_str(),
             design_hash_hex().c_str(),
             n_dpi_funcs_, scan_chain_length_, n_memories_, fifo_entry_words_);

    return {};
}

Result<void> Context::probe_rm() {
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

    // Read DPI FIFO entry words (0 if no FIFO present)
    // CONTROL register at func_idx=1022: {entry_words[31:16], threshold[15:0]}
    // When no FIFO is present, regfile returns 0xDEAD_BEEF for unknown addresses.
    val = read32(addr::DpiRegfile + reg::DpiFifoControl);
    if (val.ok() && val.value() != 0xDEADBEEF) {
        uint32_t ew = (val.value() >> 16) & 0xFFFF;
        fifo_entry_words_ = (ew > 0 && ew <= 16) ? ew : 0;
    } else {
        fifo_entry_words_ = 0;  // No FIFO or read failed
    }

    // Read 8-word design hash
    for (int i = 0; i < 8; i++) {
        val = read32(addr::EmuCtrl + reg::DesignHash0 + i * 4);
        if (!val.ok()) return val.error();
        design_hash_[i] = val.value();
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
// Clock Configuration (Clocking Wizard DRP)
// ============================================================================

Result<bool> Context::is_clock_locked() {
    auto val = read32(addr::ClkGen + reg::ClkWizStatus);
    if (!val.ok()) return val.error();
    return (val.value() & 0x1) != 0;
}

Result<void> Context::configure_clock(uint32_t freq_mhz) {
    if (freq_mhz == 0 || freq_mhz > 800) {
        logger.error("Invalid clock frequency: %u MHz", freq_mhz);
        return Error::InvalidArg;
    }

    // Calculate MMCM parameters for 300 MHz input → freq_mhz output.
    // VCO = 300 × MULT must be in 800–1600 MHz range.
    // Output = VCO / DIVIDE = 300 × MULT / DIVIDE = freq_mhz.
    //
    // Search for MULT/DIVIDE pair that produces exact frequency.
    // MULT range: 2–64, DIVIDE range: 1–128 (Xilinx 7-series/UltraScale).
    uint32_t best_mult = 0, best_div = 0;
    uint32_t best_err = UINT32_MAX;

    for (uint32_t mult = 2; mult <= 64; mult++) {
        uint32_t vco = 300 * mult;
        if (vco < 800 || vco > 1600) continue;

        // DIVIDE = VCO / freq_mhz (must be integer for exact match)
        if (vco % freq_mhz == 0) {
            uint32_t div = vco / freq_mhz;
            if (div >= 1 && div <= 128) {
                best_mult = mult;
                best_div = div;
                best_err = 0;
                break;
            }
        }

        // Nearest integer divide
        uint32_t div = (vco + freq_mhz / 2) / freq_mhz;
        if (div < 1) div = 1;
        if (div > 128) continue;
        uint32_t actual = vco / div;
        uint32_t err = (actual > freq_mhz) ? actual - freq_mhz : freq_mhz - actual;
        if (err < best_err) {
            best_err = err;
            best_mult = mult;
            best_div = div;
        }
    }

    if (best_mult == 0) {
        logger.error("Cannot find MMCM parameters for %u MHz", freq_mhz);
        return Error::InvalidArg;
    }

    uint32_t actual_freq = (300 * best_mult) / best_div;
    if (best_err > 0) {
        logger.warning("Requested %u MHz, closest achievable: %u MHz (MULT=%u, DIV=%u)",
                       freq_mhz, actual_freq, best_mult, best_div);
    } else {
        logger.info("Clock: %u MHz (MULT=%u, DIV=%u)", actual_freq, best_mult, best_div);
    }

    // Write CLKFBOUT_MULT and DIVCLK_DIVIDE to ClkWizCfg0
    // PG065 format: [15:8] = CLKFBOUT_MULT, [7:0] = DIVCLK_DIVIDE
    uint32_t cfg0 = ((best_mult & 0xFF) << 8) | (1 & 0xFF);  // DIVCLK_DIVIDE=1
    auto rc = write32(addr::ClkGen + reg::ClkWizCfg0, cfg0);
    if (!rc.ok()) return rc;

    // Write CLKOUT0_DIVIDE to ClkWizCfg2
    rc = write32(addr::ClkGen + reg::ClkWizCfg2, best_div);
    if (!rc.ok()) return rc;

    // Trigger reconfiguration: write SEN bit (bit 1) + LOAD bit (bit 0) to Cfg23
    rc = write32(addr::ClkGen + reg::ClkWizCfg23, 0x3);
    if (!rc.ok()) return rc;

    // Poll for lock with timeout (100ms)
    for (int i = 0; i < 100; i++) {
        auto locked = is_clock_locked();
        if (locked.ok() && locked.value()) {
            logger.info("Clock locked at %u MHz", actual_freq);
            return {};
        }
        usleep(1000);  // 1ms
    }

    logger.error("Clock failed to lock after reconfiguration");
    return Error::Timeout;
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
// DPI FIFO
// ============================================================================

Result<uint32_t> Context::fifo_status() {
    return read32(addr::DpiRegfile + reg::DpiFifoStatus);
}

Result<bool> Context::fifo_is_empty() {
    auto val = fifo_status();
    if (!val.ok()) return val.error();
    return (val.value() & 0x1) != 0;  // bit 0 = empty
}

Result<std::vector<uint32_t>> Context::fifo_pop_entry() {
    if (fifo_entry_words_ == 0)
        return Error::NotSupported;

    std::vector<uint32_t> data(fifo_entry_words_);
    // Read head entry data words (at ARG0 + k*4)
    for (uint32_t k = 0; k < fifo_entry_words_; k++) {
        auto val = read32(addr::DpiRegfile + reg::DpiFifoData + k * 4);
        if (!val.ok()) return val.error();
        data[k] = val.value();
    }

    // Pop: write bit0 to CONTROL register
    auto rc = write32(addr::DpiRegfile + reg::DpiFifoControl, 0x1);
    if (!rc.ok()) return rc.error();

    return data;
}

Result<void> Context::fifo_set_threshold(uint32_t level) {
    return write32(addr::DpiRegfile + reg::DpiFifoThreshold, level);
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

// ============================================================================
// PCIe-based Partial Reconfiguration
// ============================================================================

Result<void> Context::reconfigure(std::string_view partial_bit_path) {
    // Open and read the partial bitstream file
    std::string path(partial_bit_path);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        logger.error("reconfigure: cannot open '%s': %s", path.c_str(), std::strerror(errno));
        return Error::InvalidArg;
    }

    std::fseek(f, 0, SEEK_END);
    long file_size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    if (file_size <= 0) {
        std::fclose(f);
        logger.error("reconfigure: empty or unreadable file '%s'", path.c_str());
        return Error::InvalidArg;
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose(f);
        logger.error("reconfigure: short read on '%s'", path.c_str());
        return Error::Transport;
    }
    std::fclose(f);

    // Locate the bitstream sync word 0xAA995566 (UG570).
    // Everything before it (including the .bit file header) is skipped.
    size_t sync_off = buf.size();  // sentinel: not found
    for (size_t i = 0; i + 4 <= buf.size(); i++) {
        if (buf[i] == 0xAA && buf[i+1] == 0x99 &&
            buf[i+2] == 0x55 && buf[i+3] == 0x66) {
            sync_off = i;
            break;
        }
    }
    if (sync_off == buf.size()) {
        logger.error("reconfigure: sync word 0xAA995566 not found in '%s'", path.c_str());
        return Error::InvalidArg;
    }

    const uint8_t* data    = buf.data() + sync_off;
    size_t         data_sz = buf.size() - sync_off;
    size_t         n_words = (data_sz + 3) / 4;

    logger.info("reconfigure: '%s' — %zu bytes (header %zu bytes skipped)",
                path.c_str(), data_sz, sync_off);

    // 1. Decouple RP to isolate it during configuration
    auto rc = decouple();
    if (!rc.ok()) {
        logger.error("reconfigure: decouple failed");
        return rc;
    }

    // 2. Reset ICAP state machine and clear sticky status bits
    auto wr = write32(addr::IcapCtrl + reg::IcapCtrl, 0x1);  // assert sw_reset
    if (!wr.ok()) { couple(); return wr.error(); }
    wr = write32(addr::IcapCtrl + reg::IcapCtrl, 0x0);        // deassert
    if (!wr.ok()) { couple(); return wr.error(); }

    // 3. Stream bitstream words to the DATA register.
    // The .bit file stores 32-bit words big-endian.  ICAPE3 expects file byte 0
    // at I[31:24], byte 1 at I[23:16], etc. (UG570 §9, Table 9-24).  We pack
    // each 4-byte group big-endian into a uint32_t; hardware applies per-byte
    // bit-reversal before presenting to ICAPE3 (UG570 Table 2-7).
    {
        constexpr int BAR_WIDTH = 40;
        auto t0 = std::chrono::steady_clock::now();
        int last_pct = -1;

        // Print initial bar so cursor is on the right line.
        std::string empty_bar(static_cast<size_t>(BAR_WIDTH), ' ');
        std::fprintf(stderr, "  PR [%s]   0%%  --.- MB/s", empty_bar.c_str());

        for (size_t w = 0; w < n_words; w++) {
            uint32_t word = 0;
            for (int b = 0; b < 4; b++) {
                size_t off = w * 4 + static_cast<size_t>(b);
                word |= static_cast<uint32_t>(off < data_sz ? data[off] : 0x00u) << ((3 - b) * 8);
            }
            wr = write32(addr::IcapCtrl + reg::IcapData, word);
            if (!wr.ok()) {
                std::fprintf(stderr, "\n");
                logger.error("reconfigure: write failed at word %zu", w);
                couple();
                return wr.error();
            }

            int pct = static_cast<int>(100 * w / n_words);
            if (pct != last_pct) {
                last_pct = pct;
                int filled = pct * BAR_WIDTH / 100;
                auto now = std::chrono::steady_clock::now();
                double elapsed_s = std::chrono::duration<double>(now - t0).count();
                double mb_s = elapsed_s > 0.0
                    ? (static_cast<double>(w) * 4.0 / 1e6) / elapsed_s
                    : 0.0;
                // Build bar: █ = U+2588 = \xe2\x96\x88, space for empty
                std::string bar;
                bar.reserve(static_cast<size_t>(BAR_WIDTH) * 3);
                for (int i = 0; i < BAR_WIDTH; i++) {
                    if (i < filled) { bar += "\xe2\x96\x88"; }  // █
                    else            { bar += ' '; }
                }
                std::fprintf(stderr, "\r  PR [%s] %3d%%  %5.1f MB/s",
                             bar.c_str(), pct, mb_s);
                std::fflush(stderr);
            }
        }
        std::fprintf(stderr, "\n");
    }

    // 4. Poll STATUS for PRDONE (bit 1) or PRERROR (bit 2).
    // Give ICAP up to 1 s to complete configuration.
    bool pr_done  = false;
    bool pr_error = false;
    for (int i = 0; i < 100; i++) {
        auto st = read32(addr::IcapCtrl + reg::IcapStatus);
        if (!st.ok()) { couple(); return st.error(); }
        uint32_t s = st.value();
        if (s & 0x4u) { pr_error = true; break; }
        if (s & 0x2u) { pr_done  = true; break; }
        ::usleep(10000);  // 10 ms
    }

    // Final status read — PRERROR can arrive after PRDONE, so always re-check.
    {
        auto st = read32(addr::IcapCtrl + reg::IcapStatus);
        if (st.ok()) {
            uint32_t s = st.value();
            if (s & 0x4u) pr_error = true;
            if (s & 0x2u) pr_done  = true;
            logger.info("reconfigure: ICAP STATUS = 0x%02x "
                        "(busy=%d prdone=%d prerror=%d)",
                        s, !!(s & 0x1u), !!(s & 0x2u), !!(s & 0x4u));
        }
    }

    if (pr_error) {
        logger.error("reconfigure: ICAP PRERROR — partial reconfiguration failed.\n"
                     "  Common causes:\n"
                     "  - Partial bitstream built against a different static design\n"
                     "  - Wrong .bit file (full bitstream instead of partial)\n"
                     "  - Bitstream byte-ordering mismatch");
        couple();
        return Error::Transport;
    }
    if (!pr_done)
        logger.warning("reconfigure: PRDONE not seen within 1 s — PR may still succeed");

    // 5. Re-couple RP
    rc = couple();
    if (!rc.ok()) {
        logger.error("reconfigure: re-couple failed");
        return rc;
    }

    // 6. Re-read RM registers so cached shell_version / design_hash / etc.
    //    reflect the newly loaded design.
    rc = probe_rm();
    if (!rc.ok()) {
        logger.error("reconfigure: post-PR register probe failed");
        return rc;
    }

    // 6b. Verify hardware design hash matches the .hash sidecar written at
    //     build time.  A mismatch means the bitstream wasn't applied (e.g. it
    //     was built against a different static_routed.dcp and ICAP silently
    //     accepted it, or the wrong .bit file was given).
    {
        std::string hash_path = path;
        if (hash_path.size() > 4 &&
            hash_path.compare(hash_path.size() - 4, 4, ".bit") == 0)
            hash_path = hash_path.substr(0, hash_path.size() - 4) + ".hash";
        else
            hash_path += ".hash";

        FILE* hf = std::fopen(hash_path.c_str(), "r");
        if (!hf) {
            logger.warning("reconfigure: no .hash sidecar found at '%s' — "
                           "cannot verify bitstream acceptance", hash_path.c_str());
        } else {
            char expected[65] = {};
            if (std::fread(expected, 1, 64, hf) < 8) {
                logger.warning("reconfigure: .hash sidecar '%s' is malformed",
                               hash_path.c_str());
            } else {
                expected[64] = '\0';
                std::string actual = design_hash_hex();
                if (actual == std::string(expected)) {
                    logger.info("reconfigure: hash verified (%-.16s...)", expected);
                } else {
                    logger.error("reconfigure: HASH MISMATCH — bitstream may not have "
                                 "been applied!\n"
                                 "  expected: %s\n"
                                 "  actual:   %s",
                                 expected, actual.c_str());
                }
            }
            std::fclose(hf);
        }
    }

    // 7. Issue CMD_RESET so the new RM's emu_ctrl starts from a clean state.
    //    emu_rst_n is driven by the static reset synchronizer which doesn't
    //    toggle during PR — this is the only way to reset the new RM's logic.
    rc = reset();
    if (!rc.ok()) {
        logger.warning("reconfigure: post-PR reset failed (non-fatal)");
    }

    logger.info("reconfigure: done — Shell: %s, Hash: %.16s...",
                version_string(shell_version_).c_str(),
                design_hash_hex().c_str());
    return {};
}

} // namespace loom
