// SPDX-License-Identifier: Apache-2.0
// Loom Interactive Shell Implementation

#include "loom_shell.h"
#include "loom_log.h"

#include <replxx.hxx>

#include <algorithm>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>
#include <unistd.h>

namespace loom {

static Logger logger = make_logger("shell");

// ============================================================================
// SIGINT handling for the `run` command
// ============================================================================

static std::atomic<bool>* g_interrupted_flag = nullptr;

static void sigint_handler(int) {
    if (g_interrupted_flag) {
        g_interrupted_flag->store(true);
    }
}

// ============================================================================
// Tokenizer
// ============================================================================

static std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// ============================================================================
// State name helper
// ============================================================================

static const char* state_name(State s) {
    switch (s) {
    case State::Idle:     return "Idle";
    case State::Running:  return "Running";
    case State::Frozen:   return "Frozen";
    case State::Snapshot: return "Snapshot";
    case State::Restore:  return "Restore";
    case State::Error:    return "Error";
    default:              return "Unknown";
    }
}

// ============================================================================
// Shell Construction
// ============================================================================

Shell::Shell(Context& ctx, DpiService& dpi_service)
    : ctx_(ctx), dpi_service_(dpi_service), rx_(std::make_unique<replxx::Replxx>()) {
    register_commands();
    setup_replxx();
}

Shell::~Shell() {
    // Save history on destruction
    rx_->history_save(history_path());
}

// ============================================================================
// Scan Map Loading
// ============================================================================

void Shell::load_scan_map(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        logger.debug("No scan map at %s", path.c_str());
        return;
    }

    if (!scan_map_.ParseFromIstream(&f)) {
        logger.warning("Failed to parse scan map: %s", path.c_str());
        return;
    }

    scan_map_loaded_ = true;
    logger.debug("Loaded scan map: %d variables, %u bits",
                 scan_map_.variables_size(), scan_map_.chain_length());

    // Unpack initial scan image if present
    const auto& img = scan_map_.initial_scan_image();
    if (!img.empty()) {
        size_t n_words = img.size() / 4;
        initial_scan_image_.resize(n_words);
        for (size_t i = 0; i < n_words; i++) {
            initial_scan_image_[i] =
                static_cast<uint8_t>(img[i * 4 + 0])
              | (static_cast<uint8_t>(img[i * 4 + 1]) << 8)
              | (static_cast<uint8_t>(img[i * 4 + 2]) << 16)
              | (static_cast<uint8_t>(img[i * 4 + 3]) << 24);
        }
        has_initial_image_ = true;
        logger.debug("Initial scan image: %zu words", n_words);
    }

    // Unpack reset DPI mappings
    for (const auto& m : scan_map_.reset_dpi_mappings()) {
        reset_dpi_mappings_.push_back({m.func_id(), m.scan_offset(), m.scan_width()});
        logger.debug("Reset DPI mapping: func_id=%u scan[%u:%u]",
                      m.func_id(), m.scan_offset(), m.scan_offset() + m.scan_width() - 1);
    }
}

// ============================================================================
// Value Extraction
// ============================================================================

uint64_t Shell::extract_variable(const std::vector<uint32_t>& raw,
                                 uint32_t offset, uint32_t width) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < width && i < 64; i++) {
        uint32_t chain_pos = offset + i;
        uint32_t word_idx = chain_pos / 32;
        uint32_t bit_in_word = chain_pos % 32;
        if (word_idx < raw.size() && ((raw[word_idx] >> bit_in_word) & 1)) {
            value |= uint64_t(1) << i;
        }
    }
    return value;
}

std::string Shell::format_hex(uint64_t value, uint32_t width) {
    // Number of hex digits needed
    int hex_digits = (width + 3) / 4;
    if (hex_digits < 1) hex_digits = 1;

    char buf[32];
    if (width <= 32) {
        std::snprintf(buf, sizeof(buf), "0x%0*x", hex_digits,
                      static_cast<uint32_t>(value));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%0*llx", hex_digits,
                      static_cast<unsigned long long>(value));
    }
    return buf;
}

std::string Shell::format_value(const ScanVariable& var, uint64_t value) {
    for (const auto& mem : var.enum_members()) {
        if (mem.value() == value)
            return mem.name() + " (" + format_hex(value, var.width()) + ")";
    }
    return format_hex(value, var.width());
}

// ============================================================================
// Command Registration
// ============================================================================

void Shell::register_commands() {
    commands_.push_back({
        "run", {"r"},
        "Start/resume emulation",
        "Usage: run [-a] [<N>ns | <N>]\n"
        "  Release DUT reset (first time), start emulation, and service\n"
        "  DPI calls. Press Ctrl+C to interrupt and return to the shell.\n"
        "  -a         Run indefinitely (set time compare to max)\n"
        "  <N>ns      Run for N time units from current time\n"
        "  <N>        Run for N time units from current time\n"
        "  (no args)  Same as -a (run indefinitely)",
        [this](const auto& args) { return cmd_run(args); }
    });
    commands_.push_back({
        "stop", {},
        "Freeze emulation",
        "Usage: stop\n"
        "  Freeze the emulation clock. DUT state is preserved.",
        [this](const auto& args) { return cmd_stop(args); }
    });
    commands_.push_back({
        "step", {"s"},
        "Step N cycles (default 1)",
        "Usage: step [N]\n"
        "  Step the emulation by N clock cycles (default 1).\n"
        "  DPI calls are serviced during stepping.",
        [this](const auto& args) { return cmd_step(args); }
    });
    commands_.push_back({
        "status", {"st"},
        "Show emulation status",
        "Usage: status\n"
        "  Print emulation state, cycle count, design info, and DPI stats.",
        [this](const auto& args) { return cmd_status(args); }
    });
    commands_.push_back({
        "dump", {"d"},
        "Capture and display scan chain",
        "Usage: dump [<file.pb>]\n"
        "  Stop emulation if running, perform scan capture, and display\n"
        "  the captured scan chain data with named variables.\n"
        "  If a filename is given, serialize a Snapshot protobuf to that file.",
        [this](const auto& args) { return cmd_dump(args); }
    });
    commands_.push_back({
        "reset", {},
        "Assert DUT reset",
        "Usage: reset\n"
        "  Assert the DUT reset signal. Use 'run' to release and restart.",
        [this](const auto& args) { return cmd_reset(args); }
    });
    commands_.push_back({
        "read", {},
        "Read a register",
        "Usage: read <addr>\n"
        "  Read a 32-bit register at the given hex address.\n"
        "  Example: read 0x34",
        [this](const auto& args) { return cmd_read(args); }
    });
    commands_.push_back({
        "write", {"wr"},
        "Write a register",
        "Usage: write <addr> <data>\n"
        "  Write a 32-bit value to the given hex address.\n"
        "  Example: write 0x04 0x01",
        [this](const auto& args) { return cmd_write(args); }
    });
    commands_.push_back({
        "inspect", {},
        "Inspect a saved snapshot",
        "Usage: inspect <file.pb> [<var>]\n"
        "  Load a Snapshot protobuf and display metadata + variable values.\n"
        "  If <var> is given, filter variables by name prefix.",
        [this](const auto& args) { return cmd_inspect(args); }
    });
    commands_.push_back({
        "deposit_script", {},
        "Generate $deposit SystemVerilog from snapshot",
        "Usage: deposit_script <file.pb> [<output.sv>]\n"
        "  Generate SystemVerilog $deposit statements from a snapshot file.\n"
        "  Paths come from the original HDL hierarchy stored in the scan map.\n"
        "  If no output file is given, prints to stdout.",
        [this](const auto& args) { return cmd_deposit_script(args); }
    });
    commands_.push_back({
        "help", {"h", "?"},
        "Show help",
        "Usage: help [command]\n"
        "  Without arguments, list all commands.\n"
        "  With a command name, show detailed help for that command.",
        [this](const auto& args) { return cmd_help(args); }
    });
    commands_.push_back({
        "couple", {},
        "Couple decoupler (connect emu_top)",
        "Usage: couple\n"
        "  Clear the decoupler to allow AXI traffic to reach loom_emu_top.",
        [this](const auto& args) { return cmd_couple(args); }
    });
    commands_.push_back({
        "decouple", {},
        "Decouple (isolate emu_top)",
        "Usage: decouple\n"
        "  Assert the decoupler to isolate loom_emu_top from AXI traffic.\n"
        "  Transactions to the emu_top range will return SLVERR.",
        [this](const auto& args) { return cmd_decouple(args); }
    });
    commands_.push_back({
        "exit", {"quit", "q"},
        "Disconnect and exit",
        "Usage: exit\n"
        "  Cleanly disconnect from the simulation and exit the shell.",
        [this](const auto& args) { return cmd_exit(args); }
    });
}

const Command* Shell::find_command(const std::string& name) const {
    for (const auto& cmd : commands_) {
        if (cmd.name == name) return &cmd;
        for (const auto& alias : cmd.aliases) {
            if (alias == name) return &cmd;
        }
    }
    return nullptr;
}

// ============================================================================
// Replxx Setup
// ============================================================================

std::string Shell::history_path() const {
    const char* home = getenv("HOME");
    if (home) {
        return std::string(home) + "/.loom_history";
    }
    return ".loom_history";
}

void Shell::setup_replxx() {
    rx_->history_load(history_path());
    rx_->set_max_history_size(1000);

    // Tab completion
    rx_->set_completion_callback(
        [this](const std::string& input, int& context_len) {
            replxx::Replxx::completions_t completions;

            // Check if we're completing the second argument of certain commands
            auto tokens = tokenize(input);
            if (tokens.size() >= 1) {
                const auto *cmd = find_command(tokens[0]);
                bool is_file_cmd = cmd && (cmd->name == "dump" ||
                                           cmd->name == "inspect" ||
                                           cmd->name == "deposit_script");

                if (is_file_cmd && tokens.size() >= 2) {
                    // Complete *.pb files — not implemented here (filesystem
                    // completion would require OS-specific listing). Return
                    // empty to let the shell fall through.
                    return completions;
                }
            }

            context_len = static_cast<int>(input.size());
            for (const auto& cmd : commands_) {
                if (cmd.name.compare(0, input.size(), input) == 0) {
                    completions.push_back(cmd.name);
                }
                for (const auto& alias : cmd.aliases) {
                    if (alias.compare(0, input.size(), input) == 0) {
                        completions.push_back(alias);
                    }
                }
            }
            return completions;
        }
    );

    // Hints
    rx_->set_hint_callback(
        [this](const std::string& input, int& context_len, replxx::Replxx::Color& color) {
            replxx::Replxx::hints_t hints;
            if (input.empty()) return hints;
            context_len = static_cast<int>(input.size());
            color = replxx::Replxx::Color::GRAY;
            for (const auto& cmd : commands_) {
                if (cmd.name.compare(0, input.size(), input) == 0) {
                    hints.push_back(cmd.name + " -- " + cmd.brief);
                }
            }
            return hints;
        }
    );

    // Syntax highlighting
    rx_->set_highlighter_callback(
        [this](const std::string& input, replxx::Replxx::colors_t& colors) {
            auto tokens = tokenize(input);
            if (tokens.empty()) return;
            // Determine the length of the first token in the input
            size_t first_end = input.find_first_of(" \t");
            if (first_end == std::string::npos) first_end = input.size();
            auto color = find_command(tokens[0])
                ? replxx::Replxx::Color::GREEN
                : replxx::Replxx::Color::RED;
            for (size_t i = 0; i < first_end && i < colors.size(); i++) {
                colors[i] = color;
            }
        }
    );
}

// ============================================================================
// Command Execution
// ============================================================================

int Shell::execute(const std::string& line) {
    auto tokens = tokenize(line);
    if (tokens.empty()) return 0;

    const auto* cmd = find_command(tokens[0]);
    if (!cmd) {
        logger.error("Unknown command: '%s'. Type 'help' for a list.", tokens[0].c_str());
        return -1;
    }

    return cmd->handler(tokens);
}

// ============================================================================
// REPL Loop
// ============================================================================

int Shell::run_interactive() {
    logger.info("Loom interactive shell. Type 'help' for commands.");

    while (!exit_requested_) {
        const char* input = rx_->input("loom> ");
        if (input == nullptr) {
            // EOF (Ctrl+D)
            break;
        }

        std::string line(input);
        if (line.empty()) continue;

        rx_->history_add(line);
        execute(line);
    }

    return 0;
}

// ============================================================================
// Script Mode
// ============================================================================

int Shell::run_script(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        logger.error("Cannot open script: %s", filename.c_str());
        return 1;
    }

    logger.info("Running script: %s", filename.c_str());

    std::string line;
    int line_num = 0;
    while (std::getline(file, line)) {
        line_num++;
        // Skip comments and blank lines
        auto pos = line.find_first_not_of(" \t");
        if (pos == std::string::npos) continue;
        if (line[pos] == '#') continue;

        logger.info("[%d] %s", line_num, line.c_str());
        int rc = execute(line);
        if (exit_requested_) break;
        if (rc < 0) {
            logger.error("Script failed at line %d", line_num);
            return 1;
        }
    }

    return 0;
}

// ============================================================================
// Command: run
// ============================================================================

int Shell::cmd_run(const std::vector<std::string>& args) {
    // Parse arguments: run [-a] [<N>ns | <N>]
    uint64_t time_cmp = UINT64_MAX;  // default: run indefinitely

    if (args.size() > 1 && args[1] != "-a") {
        std::string arg = args[1];
        // Strip optional "ns" suffix
        if (arg.size() > 2 && arg.substr(arg.size() - 2) == "ns") {
            arg = arg.substr(0, arg.size() - 2);
        }
        uint64_t delta = std::strtoull(arg.c_str(), nullptr, 10);

        auto cur_time = ctx_.get_time();
        if (!cur_time.ok()) {
            logger.error("Failed to get current time");
            return -1;
        }
        time_cmp = cur_time.value() + delta;
    }

    // Set time compare before starting
    auto tc_rc = ctx_.set_time_compare(time_cmp);
    if (!tc_rc.ok()) {
        logger.error("Failed to set time compare");
        return -1;
    }

    // Ensure decoupler is coupled before starting
    ctx_.couple();

    // Release DUT reset and start emulation
    auto state_result = ctx_.get_state();
    if (!state_result.ok()) {
        logger.error("Failed to get state");
        return -1;
    }

    if (state_result.value() == State::Idle || state_result.value() == State::Frozen) {
        // Scan-based init: execute initial DPI calls and scan in image before first run
        if (has_initial_image_ && !initial_dpi_executed_) {
            execute_initial_dpi_calls();
        }
        if (has_initial_image_ && !initial_image_applied_) {
            logger.info("Scanning in initial state...");
            ctx_.scan_write_data(initial_scan_image_);
            ctx_.scan_restore();
            initial_image_applied_ = true;
        }
        auto rc = ctx_.start();
        if (!rc.ok()) {
            logger.error("Failed to start emulation");
            return -1;
        }
        logger.info("Emulation started");
    }

    // Install SIGINT handler
    interrupted_.store(false);
    g_interrupted_flag = &interrupted_;

    struct sigaction sa{}, old_sa{};
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &old_sa);

    // Service DPI calls until interrupted or emulation stops
    while (!interrupted_.load()) {
        int rc = dpi_service_.service_once(ctx_);
        if (rc == static_cast<int>(Error::Shutdown)) {
            logger.info("Shutdown received");
            break;
        }
        if (rc < 0) {
            logger.error("DPI service error");
            break;
        }

        auto st = ctx_.get_state();
        if (!st.ok()) {
            if (st.error() == Error::Shutdown) {
                logger.info("Shutdown received");
                break;
            }
            logger.error("Failed to get state");
            break;
        }

        if (st.value() == State::Frozen) {
            logger.info("Emulation frozen");
            break;
        }
        if (st.value() == State::Error) {
            logger.error("Emulation error state");
            break;
        }

        usleep(1000);  // 1ms
    }

    // Restore original SIGINT handler
    sigaction(SIGINT, &old_sa, nullptr);
    g_interrupted_flag = nullptr;

    if (interrupted_.load()) {
        ctx_.stop();
        logger.info("Interrupted");
    }

    // Print cycle count and DUT time
    auto cycles = ctx_.get_cycle_count();
    if (cycles.ok()) {
        logger.info("Cycle count: %llu",
                    static_cast<unsigned long long>(cycles.value()));
    }
    auto time_val = ctx_.get_time();
    if (time_val.ok()) {
        logger.info("DUT time: %llu",
                    static_cast<unsigned long long>(time_val.value()));
    }

    return 0;
}

// ============================================================================
// Command: stop
// ============================================================================

int Shell::cmd_stop(const std::vector<std::string>& /*args*/) {
    auto rc = ctx_.stop();
    if (!rc.ok()) {
        logger.error("Failed to stop emulation");
        return -1;
    }
    logger.info("Emulation stopped");
    return 0;
}

// ============================================================================
// Command: step
// ============================================================================

int Shell::cmd_step(const std::vector<std::string>& args) {
    uint32_t n = 1;
    if (args.size() > 1) {
        n = static_cast<uint32_t>(std::stoul(args[1]));
    }

    // Scan-based init on first step, or release DUT reset (legacy)
    if (has_initial_image_ && !initial_dpi_executed_) {
        execute_initial_dpi_calls();
    }
    if (has_initial_image_ && !initial_image_applied_) {
        logger.info("Scanning in initial state...");
        ctx_.scan_write_data(initial_scan_image_);
        ctx_.scan_restore();
        initial_image_applied_ = true;
    }

    // SW-based step: set time_cmp = time + N, then CMD_START
    auto rc = ctx_.step(n);
    if (!rc.ok()) {
        logger.error("Failed to step");
        return -1;
    }

    // Wait for DUT to reach time_cmp (state transitions Running → Frozen)
    // and service DPI calls during the run
    while (true) {
        int svc = dpi_service_.service_once(ctx_);
        if (svc == static_cast<int>(Error::Shutdown)) {
            logger.info("Shutdown received during step");
            break;
        }

        auto st = ctx_.get_state();
        if (!st.ok()) break;
        if (st.value() != State::Running) break;
        usleep(1000);
    }

    auto cycles = ctx_.get_cycle_count();
    if (cycles.ok()) {
        logger.info("Stepped %u cycle%s (total: %llu)", n, n == 1 ? "" : "s",
                    static_cast<unsigned long long>(cycles.value()));
    }

    return 0;
}

// ============================================================================
// Command: status
// ============================================================================

int Shell::cmd_status(const std::vector<std::string>& /*args*/) {
    auto st = ctx_.get_state();
    if (!st.ok()) {
        logger.error("Failed to get state");
        return -1;
    }

    auto cycles = ctx_.get_cycle_count();
    uint64_t cycle_count = cycles.ok() ? cycles.value() : 0;

    auto time_result = ctx_.get_time();
    uint64_t dut_time = time_result.ok() ? time_result.value() : 0;

    auto time_cmp_result = ctx_.get_time_compare();
    uint64_t time_cmp = time_cmp_result.ok() ? time_cmp_result.value() : 0;

    std::printf("  State:       %s\n", state_name(st.value()));
    std::printf("  Cycles:      %llu\n", static_cast<unsigned long long>(cycle_count));
    std::printf("  DUT time:    %llu\n", static_cast<unsigned long long>(dut_time));
    if (time_cmp == UINT64_MAX) {
        std::printf("  Time cmp:    unlimited\n");
    } else {
        std::printf("  Time cmp:    %llu\n", static_cast<unsigned long long>(time_cmp));
    }
    std::printf("  Design ID:   0x%08x\n", ctx_.design_id());
    std::printf("  Loom ver:    0x%08x\n", ctx_.loom_version());
    std::printf("  DPI funcs:   %u\n", ctx_.n_dpi_funcs());
    std::printf("  Scan bits:   %u\n", ctx_.scan_chain_length());
    std::printf("  DPI calls:   %llu\n", static_cast<unsigned long long>(dpi_service_.call_count()));
    std::printf("  DPI errors:  %llu\n", static_cast<unsigned long long>(dpi_service_.error_count()));

    return 0;
}

// ============================================================================
// Command: dump
// ============================================================================

int Shell::cmd_dump(const std::vector<std::string>& args) {
    if (ctx_.scan_chain_length() == 0) {
        logger.info("No scan chain in design");
        return 0;
    }

    // Scan-based init: apply initial image if no reset DPI mappings need
    // to patch it.  When reset DPI exists, defer to first step/run.
    if (has_initial_image_ && !initial_image_applied_ && reset_dpi_mappings_.empty()) {
        logger.info("Scanning in initial state...");
        ctx_.scan_write_data(initial_scan_image_);
        ctx_.scan_restore();
        initial_image_applied_ = true;
    }

    // Stop if running
    auto st = ctx_.get_state();
    if (st.ok() && st.value() == State::Running) {
        ctx_.stop();
        logger.info("Stopped for scan capture");
    }

    // Capture
    auto rc = ctx_.scan_capture(5000);
    if (!rc.ok()) {
        logger.error("Scan capture failed");
        return -1;
    }

    // Read raw data
    auto data = ctx_.scan_read_data();
    if (!data.ok()) {
        logger.error("Failed to read scan data");
        return -1;
    }

    const auto& scan = data.value();
    std::printf("  Scan chain: %u bits (%zu words)\n",
                ctx_.scan_chain_length(), scan.size());

    // Display named variables if scan map is loaded
    if (scan_map_loaded_ && scan_map_.variables_size() > 0) {
        // Find max name length for alignment
        size_t max_name = 0;
        for (const auto& var : scan_map_.variables()) {
            max_name = std::max(max_name, var.name().size());
        }

        for (const auto& var : scan_map_.variables()) {
            uint64_t val = extract_variable(scan, var.offset(), var.width());
            std::printf("  %-*s [%2u] = %s\n",
                        static_cast<int>(max_name), var.name().c_str(),
                        var.width(), format_value(var, val).c_str());
        }
    } else {
        // Fallback: raw words
        for (size_t i = 0; i < scan.size(); i++) {
            std::printf("  [%2zu] 0x%08x\n", i, scan[i]);
        }
    }

    // Save snapshot to file if requested
    if (args.size() > 1) {
        const std::string& filename = args[1];

        Snapshot snapshot;

        auto cycles = ctx_.get_cycle_count();
        if (cycles.ok())
            snapshot.set_cycle_count(cycles.value());

        auto time_val = ctx_.get_time();
        if (time_val.ok())
            snapshot.set_dut_time(time_val.value());

        snapshot.set_design_id(ctx_.design_id());

        // Pack raw scan data as LE bytes
        std::string raw_bytes(scan.size() * 4, '\0');
        for (size_t i = 0; i < scan.size(); i++) {
            raw_bytes[i * 4 + 0] = static_cast<char>((scan[i] >>  0) & 0xFF);
            raw_bytes[i * 4 + 1] = static_cast<char>((scan[i] >>  8) & 0xFF);
            raw_bytes[i * 4 + 2] = static_cast<char>((scan[i] >> 16) & 0xFF);
            raw_bytes[i * 4 + 3] = static_cast<char>((scan[i] >> 24) & 0xFF);
        }
        snapshot.set_raw_scan_data(raw_bytes);

        // Embed scan map for self-contained file
        if (scan_map_loaded_) {
            *snapshot.mutable_scan_map() = scan_map_;
        }

        std::ofstream out(filename, std::ios::binary);
        if (!out.is_open()) {
            logger.error("Cannot open %s for writing", filename.c_str());
            return -1;
        }
        if (!snapshot.SerializeToOstream(&out)) {
            logger.error("Failed to serialize snapshot to %s", filename.c_str());
            return -1;
        }
        logger.info("Snapshot saved to %s", filename.c_str());
    }

    return 0;
}

// ============================================================================
// Command: inspect
// ============================================================================

int Shell::cmd_inspect(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        logger.error("Usage: inspect <file.pb> [<var>]");
        return -1;
    }

    const std::string& filename = args[1];
    std::string filter;
    if (args.size() > 2)
        filter = args[2];

    // Load snapshot
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) {
        logger.error("Cannot open %s", filename.c_str());
        return -1;
    }

    Snapshot snapshot;
    if (!snapshot.ParseFromIstream(&in)) {
        logger.error("Failed to parse snapshot: %s", filename.c_str());
        return -1;
    }

    // Display metadata
    std::printf("  File:       %s\n", filename.c_str());
    std::printf("  Cycle:      %llu\n",
                static_cast<unsigned long long>(snapshot.cycle_count()));
    std::printf("  DUT time:   %llu\n",
                static_cast<unsigned long long>(snapshot.dut_time()));
    std::printf("  Design ID:  0x%08x\n", snapshot.design_id());

    if (!snapshot.has_scan_map() || snapshot.scan_map().variables_size() == 0) {
        std::printf("  (no embedded scan map)\n");
        // Display raw data
        const auto& raw = snapshot.raw_scan_data();
        size_t n_words = raw.size() / 4;
        for (size_t i = 0; i < n_words; i++) {
            uint32_t w = static_cast<uint8_t>(raw[i * 4 + 0])
                       | (static_cast<uint8_t>(raw[i * 4 + 1]) << 8)
                       | (static_cast<uint8_t>(raw[i * 4 + 2]) << 16)
                       | (static_cast<uint8_t>(raw[i * 4 + 3]) << 24);
            std::printf("  [%2zu] 0x%08x\n", i, w);
        }
        return 0;
    }

    const auto& map = snapshot.scan_map();
    std::printf("  Chain:      %u bits, %d variables\n",
                map.chain_length(), map.variables_size());

    // Unpack raw scan data to words
    const auto& raw_bytes = snapshot.raw_scan_data();
    size_t n_words = raw_bytes.size() / 4;
    std::vector<uint32_t> raw(n_words);
    for (size_t i = 0; i < n_words; i++) {
        raw[i] = static_cast<uint8_t>(raw_bytes[i * 4 + 0])
               | (static_cast<uint8_t>(raw_bytes[i * 4 + 1]) << 8)
               | (static_cast<uint8_t>(raw_bytes[i * 4 + 2]) << 16)
               | (static_cast<uint8_t>(raw_bytes[i * 4 + 3]) << 24);
    }

    // Find max name length for alignment
    size_t max_name = 0;
    for (const auto& var : map.variables()) {
        if (filter.empty() || var.name().find(filter) == 0)
            max_name = std::max(max_name, var.name().size());
    }

    for (const auto& var : map.variables()) {
        if (!filter.empty() && var.name().find(filter) != 0)
            continue;
        uint64_t val = extract_variable(raw, var.offset(), var.width());
        std::printf("  %-*s [%2u] = %s\n",
                    static_cast<int>(max_name), var.name().c_str(),
                    var.width(), format_value(var, val).c_str());
    }

    return 0;
}

// ============================================================================
// Command: deposit_script
// ============================================================================

int Shell::cmd_deposit_script(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        logger.error("Usage: deposit_script <file.pb> [<output.sv>]");
        return -1;
    }

    const std::string& filename = args[1];

    // Load snapshot
    std::ifstream in(filename, std::ios::binary);
    if (!in.is_open()) {
        logger.error("Cannot open %s", filename.c_str());
        return -1;
    }

    Snapshot snapshot;
    if (!snapshot.ParseFromIstream(&in)) {
        logger.error("Failed to parse snapshot: %s", filename.c_str());
        return -1;
    }

    if (!snapshot.has_scan_map() || snapshot.scan_map().variables_size() == 0) {
        logger.error("Snapshot has no embedded scan map");
        return -1;
    }

    const auto& map = snapshot.scan_map();

    // Unpack raw scan data to words
    const auto& raw_bytes = snapshot.raw_scan_data();
    size_t n_words = raw_bytes.size() / 4;
    std::vector<uint32_t> raw(n_words);
    for (size_t i = 0; i < n_words; i++) {
        raw[i] = static_cast<uint8_t>(raw_bytes[i * 4 + 0])
               | (static_cast<uint8_t>(raw_bytes[i * 4 + 1]) << 8)
               | (static_cast<uint8_t>(raw_bytes[i * 4 + 2]) << 16)
               | (static_cast<uint8_t>(raw_bytes[i * 4 + 3]) << 24);
    }

    // Generate deposit statements
    FILE* out = stdout;
    std::ofstream out_file;
    if (args.size() > 2) {
        out_file.open(args[2]);
        if (!out_file.is_open()) {
            logger.error("Cannot open %s for writing", args[2].c_str());
            return -1;
        }
    }

    auto emit = [&](const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        if (out_file.is_open()) {
            char buf[512];
            std::vsnprintf(buf, sizeof(buf), fmt, ap);
            out_file << buf;
        } else {
            std::vfprintf(out, fmt, ap);
        }
        va_end(ap);
    };

    emit("// Auto-generated by loom deposit_script\n");
    emit("// Source: %s (cycle %llu)\n", filename.c_str(),
         static_cast<unsigned long long>(snapshot.cycle_count()));

    for (const auto& var : map.variables()) {
        uint64_t val = extract_variable(raw, var.offset(), var.width());
        int hex_digits = (var.width() + 3) / 4;
        if (hex_digits < 1) hex_digits = 1;

        if (var.width() <= 32) {
            emit("$deposit(%s, %u'h%0*x);\n", var.name().c_str(),
                 var.width(), hex_digits, static_cast<uint32_t>(val));
        } else {
            emit("$deposit(%s, %u'h%0*llx);\n", var.name().c_str(),
                 var.width(), hex_digits,
                 static_cast<unsigned long long>(val));
        }
    }

    if (out_file.is_open()) {
        logger.info("Deposit script written to %s", args[2].c_str());
    }

    return 0;
}

// ============================================================================
// Command: reset
// ============================================================================

int Shell::cmd_reset(const std::vector<std::string>& /*args*/) {
    // Scan-based reset: re-scan the initial image
    ctx_.stop();
    ctx_.scan_write_data(initial_scan_image_);
    ctx_.scan_restore();
    initial_image_applied_ = true;
    logger.info("DUT reset via scan chain");
    return 0;
}

// ============================================================================
// Command: read
// ============================================================================

int Shell::cmd_read(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        logger.error("Usage: read <addr>");
        return -1;
    }

    char *end = nullptr;
    uint32_t addr = static_cast<uint32_t>(std::strtoul(args[1].c_str(), &end, 16));
    if (end == args[1].c_str()) {
        logger.error("Invalid address: %s", args[1].c_str());
        return -1;
    }

    auto result = ctx_.read32(addr);
    if (!result.ok()) {
        logger.error("Read failed at 0x%05x", addr);
        return -1;
    }

    std::printf("0x%08x\n", result.value());
    return 0;
}

// ============================================================================
// Command: write
// ============================================================================

int Shell::cmd_write(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        logger.error("Usage: write <addr> <data>");
        return -1;
    }

    char *end = nullptr;
    uint32_t addr = static_cast<uint32_t>(std::strtoul(args[1].c_str(), &end, 16));
    if (end == args[1].c_str()) {
        logger.error("Invalid address: %s", args[1].c_str());
        return -1;
    }

    end = nullptr;
    uint32_t data = static_cast<uint32_t>(std::strtoul(args[2].c_str(), &end, 16));
    if (end == args[2].c_str()) {
        logger.error("Invalid data: %s", args[2].c_str());
        return -1;
    }

    auto rc = ctx_.write32(addr, data);
    if (!rc.ok()) {
        logger.error("Write failed at 0x%05x", addr);
        return -1;
    }

    std::printf("OK [0x%05x] <- 0x%08x\n", addr, data);
    return 0;
}

// ============================================================================
// Command: help
// ============================================================================

int Shell::cmd_help(const std::vector<std::string>& args) {
    if (args.size() > 1) {
        const auto* cmd = find_command(args[1]);
        if (cmd) {
            std::printf("%s\n", cmd->usage.c_str());
            if (!cmd->aliases.empty()) {
                std::printf("Aliases:");
                for (const auto& a : cmd->aliases) {
                    std::printf(" %s", a.c_str());
                }
                std::printf("\n");
            }
        } else {
            logger.error("Unknown command: '%s'", args[1].c_str());
        }
        return 0;
    }

    std::printf("Commands:\n");
    for (const auto& cmd : commands_) {
        std::string aliases_str;
        if (!cmd.aliases.empty()) {
            aliases_str = " (";
            for (size_t i = 0; i < cmd.aliases.size(); i++) {
                if (i > 0) aliases_str += ", ";
                aliases_str += cmd.aliases[i];
            }
            aliases_str += ")";
        }
        std::printf("  %-16s%s  %s\n", cmd.name.c_str(),
                    aliases_str.c_str(), cmd.brief.c_str());
    }
    return 0;
}

// ============================================================================
// Command: couple
// ============================================================================

int Shell::cmd_couple(const std::vector<std::string>& /*args*/) {
    auto rc = ctx_.couple();
    if (!rc.ok()) {
        logger.error("Failed to couple");
        return -1;
    }
    logger.info("Decoupler cleared — emu_top connected");
    return 0;
}

// ============================================================================
// Command: decouple
// ============================================================================

int Shell::cmd_decouple(const std::vector<std::string>& /*args*/) {
    auto rc = ctx_.decouple();
    if (!rc.ok()) {
        logger.error("Failed to decouple");
        return -1;
    }
    logger.info("Decoupler asserted — emu_top isolated");
    return 0;
}

// ============================================================================
// Command: exit
// ============================================================================

int Shell::cmd_exit(const std::vector<std::string>& /*args*/) {
    exit_requested_ = true;
    return 1;
}

// ============================================================================
// Execute initial/reset DPI calls before scan-in
// ============================================================================

void Shell::execute_initial_dpi_calls() {
    initial_dpi_executed_ = true;

    // Count init functions from dispatch table
    int n_init = 0;
    for (const auto& func : dpi_service_.funcs()) {
        if (func.call_at_init) n_init++;
    }

    if (n_init == 0 && reset_dpi_mappings_.empty())
        return;

    logger.info("Executing %d initial DPI call(s)...", n_init);

    // Build set of func_ids that have reset DPI mappings (called in Phase 2)
    std::set<uint32_t> reset_func_ids;
    for (const auto& mapping : reset_dpi_mappings_) {
        reset_func_ids.insert(mapping.func_id);
    }

    // Phase 1: Call void init functions from dispatch table (skip reset DPI)
    for (const auto& func : dpi_service_.funcs()) {
        if (!func.call_at_init) continue;
        if (reset_func_ids.count(func.func_id)) continue;  // handled in Phase 2

        std::vector<uint32_t> out_args(func.out_arg_words, 0);
        std::vector<uint32_t> dummy_args;
        func.callback(std::span<const uint32_t>(dummy_args),
                       std::span<uint32_t>(out_args));
        logger.info("Executed initial DPI call: %s (void)", func.name.c_str());
    }

    // Phase 2: Call reset DPI functions and patch scan image with results
    for (const auto& mapping : reset_dpi_mappings_) {
        const auto* func = dpi_service_.find_func_by_id(mapping.func_id);
        if (!func) {
            logger.warning("Reset DPI func_id %u not found in dispatch table", mapping.func_id);
            continue;
        }

        std::vector<uint32_t> out_args(func->out_arg_words, 0);
        std::vector<uint32_t> dummy_args;
        uint64_t result = func->callback(std::span<const uint32_t>(dummy_args),
                                          std::span<uint32_t>(out_args));

        // Inject result into scan image
        if (has_initial_image_) {
            for (uint32_t i = 0; i < mapping.scan_width && i < 64; i++) {
                uint32_t word_idx = (mapping.scan_offset + i) / 32;
                uint32_t bit_idx = (mapping.scan_offset + i) % 32;
                if (word_idx < initial_scan_image_.size()) {
                    if (result & (1ULL << i))
                        initial_scan_image_[word_idx] |= (1u << bit_idx);
                    else
                        initial_scan_image_[word_idx] &= ~(1u << bit_idx);
                }
            }
        }
        logger.info("Reset DPI: %s -> 0x%llx (scan[%u:%u])",
                     func->name.c_str(), static_cast<unsigned long long>(result),
                     mapping.scan_offset, mapping.scan_offset + mapping.scan_width - 1);
    }
}

} // namespace loom
