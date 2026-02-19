// SPDX-License-Identifier: Apache-2.0
// Loom Interactive Shell
//
// Provides an interactive command-line interface for controlling
// Loom-instrumented designs. Supports tab completion, hints,
// syntax highlighting, and persistent history via replxx.

#pragma once

#include "loom.h"
#include "loom_dpi_service.h"

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
    int cmd_help(const std::vector<std::string>& args);
    int cmd_exit(const std::vector<std::string>& args);

    Context& ctx_;
    DpiService& dpi_service_;
    std::vector<Command> commands_;
    std::unique_ptr<replxx::Replxx> rx_;
    std::atomic<bool> interrupted_{false};
    bool exit_requested_ = false;
};

} // namespace loom
