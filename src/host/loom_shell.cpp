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
    case State::Stepping: return "Stepping";
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
        "Usage: dump\n"
        "  Stop emulation if running, perform scan capture, and display\n"
        "  the captured scan chain data.",
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
        "help", {"h", "?"},
        "Show help",
        "Usage: help [command]\n"
        "  Without arguments, list all commands.\n"
        "  With a command name, show detailed help for that command.",
        [this](const auto& args) { return cmd_help(args); }
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

    // Release DUT reset and start emulation
    auto state_result = ctx_.get_state();
    if (!state_result.ok()) {
        logger.error("Failed to get state");
        return -1;
    }

    if (state_result.value() == State::Idle || state_result.value() == State::Frozen) {
        ctx_.dut_reset(false);
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

    // Ensure DUT reset is released
    ctx_.dut_reset(false);

    auto rc = ctx_.step(n);
    if (!rc.ok()) {
        logger.error("Failed to step");
        return -1;
    }

    // Wait for stepping to complete and service DPI calls during it
    while (true) {
        int svc = dpi_service_.service_once(ctx_);
        if (svc == static_cast<int>(Error::Shutdown)) {
            logger.info("Shutdown received during step");
            break;
        }

        auto st = ctx_.get_state();
        if (!st.ok()) break;
        if (st.value() != State::Stepping) break;
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

int Shell::cmd_dump(const std::vector<std::string>& /*args*/) {
    if (ctx_.scan_chain_length() == 0) {
        logger.info("No scan chain in design");
        return 0;
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

    // Read and display
    auto data = ctx_.scan_read_data();
    if (!data.ok()) {
        logger.error("Failed to read scan data");
        return -1;
    }

    const auto& scan = data.value();
    std::printf("  Scan chain: %u bits (%zu words)\n",
                ctx_.scan_chain_length(), scan.size());
    for (size_t i = 0; i < scan.size(); i++) {
        std::printf("  [%2zu] 0x%08x\n", i, scan[i]);
    }

    return 0;
}

// ============================================================================
// Command: reset
// ============================================================================

int Shell::cmd_reset(const std::vector<std::string>& /*args*/) {
    auto rc = ctx_.dut_reset(true);
    if (!rc.ok()) {
        logger.error("Failed to assert reset");
        return -1;
    }
    logger.info("DUT reset asserted");
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
        std::printf("  %-8s%s  %s\n", cmd.name.c_str(),
                    aliases_str.c_str(), cmd.brief.c_str());
    }
    return 0;
}

// ============================================================================
// Command: exit
// ============================================================================

int Shell::cmd_exit(const std::vector<std::string>& /*args*/) {
    exit_requested_ = true;
    return 1;
}

} // namespace loom
