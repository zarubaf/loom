// SPDX-License-Identifier: Apache-2.0
// Loom Interactive Shell
//
// Provides an interactive command-line interface for controlling
// Loom-instrumented designs. Supports tab completion, hints,
// syntax highlighting, and persistent history via replxx.

#pragma once

#include "loom.h"
#include "loom_dpi_service.h"
#include "loom_snapshot.pb.h"

#include <functional>
#include <string>
#include <vector>
#include <atomic>

namespace replxx {
class Replxx;
}

namespace loom {

// A single shell command
struct Command {
    std::string name;
    std::vector<std::string> aliases;
    std::string brief;
    std::string usage;
    std::function<int(const std::vector<std::string>&)> handler;
};

class Shell {
public:
    Shell(Context& ctx, DpiService& dpi_service);
    ~Shell();

    // Non-copyable
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // Load a scan map from a protobuf file.
    // Must be called before dump/inspect/deposit_script can decode variables.
    void load_scan_map(const std::string& path);

    // Run interactive REPL loop. Returns process exit code.
    int run_interactive();

    // Run commands from a script file. Returns process exit code.
    int run_script(const std::string& filename);

    // Execute a single command line. Returns 0 on success, negative on error,
    // 1 to signal exit.
    int execute(const std::string& line);

private:
    void register_commands();
    const Command* find_command(const std::string& name) const;

    // Replxx callbacks
    void setup_replxx();
    std::string history_path() const;

    // Built-in command handlers
    int cmd_run(const std::vector<std::string>& args);
    int cmd_stop(const std::vector<std::string>& args);
    int cmd_step(const std::vector<std::string>& args);
    int cmd_status(const std::vector<std::string>& args);
    int cmd_dump(const std::vector<std::string>& args);
    int cmd_reset(const std::vector<std::string>& args);
    int cmd_read(const std::vector<std::string>& args);
    int cmd_write(const std::vector<std::string>& args);
    int cmd_inspect(const std::vector<std::string>& args);
    int cmd_deposit_script(const std::vector<std::string>& args);
    int cmd_help(const std::vector<std::string>& args);
    int cmd_exit(const std::vector<std::string>& args);
    int cmd_couple(const std::vector<std::string>& args);
    int cmd_decouple(const std::vector<std::string>& args);

    // Value extraction helpers
    static uint64_t extract_variable(const std::vector<uint32_t>& raw,
                                     uint32_t offset, uint32_t width);
    static std::string format_hex(uint64_t value, uint32_t width);
    static std::string format_value(const ScanVariable& var, uint64_t value);

    Context& ctx_;
    DpiService& dpi_service_;
    std::vector<Command> commands_;
    std::unique_ptr<replxx::Replxx> rx_;
    std::atomic<bool> interrupted_{false};
    bool exit_requested_ = false;
    ScanMap scan_map_;
    bool scan_map_loaded_ = false;
    std::vector<uint32_t> initial_scan_image_;
    bool has_initial_image_ = false;
    bool initial_image_applied_ = false;

    // Reset DPI mappings: func_id → scan chain position
    struct ResetDpiMapping {
        uint32_t func_id;
        uint32_t scan_offset;
        uint32_t scan_width;
    };
    std::vector<ResetDpiMapping> reset_dpi_mappings_;
    bool initial_dpi_executed_ = false;

    // Execute initial/reset DPI calls and patch scan image
    void execute_initial_dpi_calls();
};

} // namespace loom
