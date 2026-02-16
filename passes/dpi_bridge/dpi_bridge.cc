// SPDX-License-Identifier: Apache-2.0
/*
 * dpi_bridge - Yosys pass for DPI-C function bridge generation
 *
 * This pass converts DPI function call cells ($__loom_dpi_call) into hardware
 * interfaces that enable FPGA<->host communication for emulation.
 *
 * IMPORTANT: DPI calls must only appear in clocked (always_ff) blocks.
 * This ensures deterministic clock gating behavior and simplifies the
 * valid condition derivation.
 *
 * Example valid usage:
 *   always_ff @(posedge clk_i or negedge rst_ni) begin
 *     if (!rst_ni) begin
 *       result_q <= '0;
 *     end else if (state_q == StCallDpi) begin
 *       result_q <= dpi_function(arg1, arg2);  // OK: in clocked block
 *     end
 *   end
 *
 * DUT bridge interface (connects to emu_top wrapper):
 *   - loom_dpi_valid:   DPI call pending (output, triggers clock gating)
 *   - loom_dpi_func_id: Function identifier (output)
 *   - loom_dpi_args:    Packed function arguments (output)
 *   - loom_dpi_result:  Return value from host (input)
 *
 * The pass also outputs:
 *   - JSON metadata for host-side integration
 *   - C header with function prototypes for user implementation
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include <fstream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Address map constants (per DPI bridge spec)
constexpr int LOOM_MAILBOX_BASE = 0x00000;
constexpr int LOOM_DPI_BASE = 0x00100;
constexpr int FUNC_BLOCK_ALIGN = 64;  // Bytes per function block

// Argument descriptor
struct DpiArg {
    std::string name;
    std::string type;      // SV type name
    std::string direction; // input, output, inout
    int width;
};

// DPI function descriptor extracted from cells
struct DpiFunction {
    std::string name;
    int func_id;
    int arg_width;      // Total packed width
    int ret_width;
    std::string ret_type;
    std::vector<DpiArg> args;
    RTLIL::Cell *cell;
    RTLIL::SigSpec args_sig;
    RTLIL::SigSpec result_sig;
    RTLIL::SigSpec valid_condition;  // Derived execution condition (populated after derive)
};

struct DpiBridgePass : public Pass {
    DpiBridgePass() : Pass("dpi_bridge", "Convert DPI placeholders to hardware bridges") {}

    void help() override {
        log("\n");
        log("    dpi_bridge [options] [selection]\n");
        log("\n");
        log("Convert DPI function call cells to hardware bridge interfaces.\n");
        log("\n");
        log("IMPORTANT: DPI calls must only appear in clocked (always_ff) blocks.\n");
        log("This ensures deterministic clock gating and correct valid derivation.\n");
        log("\n");
        log("Options:\n");
        log("    -json_out <file>\n");
        log("        Write DPI metadata to JSON file.\n");
        log("\n");
        log("    -header_out <file>\n");
        log("        Write C header file with DPI function prototypes.\n");
        log("        Users implement these functions for host-side dispatch.\n");
        log("\n");
        log("DUT ports created:\n");
        log("  - loom_dpi_valid:   DPI call pending (output, gates clock)\n");
        log("  - loom_dpi_func_id: Function identifier (output, 8-bit)\n");
        log("  - loom_dpi_args:    Packed function arguments (output)\n");
        log("  - loom_dpi_result:  Return value from host (input)\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing DPI_BRIDGE pass.\n");

        bool gen_wrapper = false;
        std::string json_out_path;
        std::string header_out_path;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-gen_wrapper") {
                gen_wrapper = true;
                continue;
            }
            if (args[argidx] == "-json_out" && argidx + 1 < args.size()) {
                json_out_path = args[++argidx];
                continue;
            }
            if (args[argidx] == "-header_out" && argidx + 1 < args.size()) {
                header_out_path = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        int func_id = 0;
        std::vector<DpiFunction> dpi_functions;

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));

            std::vector<RTLIL::Cell*> cells_to_process;
            std::vector<DpiFunction> module_functions;

            // Find $__loom_dpi_call cells created by yosys-slang
            for (auto cell : module->cells()) {
                if (cell->type == ID($__loom_dpi_call)) {
                    cells_to_process.push_back(cell);
                }
            }

            if (cells_to_process.empty()) {
                log("  No DPI call cells found.\n");
                continue;
            }

            log("  Found %zu DPI call cell(s)\n", cells_to_process.size());

            // First pass: collect all DPI function info
            for (auto cell : cells_to_process) {
                // Get function name from attribute
                std::string dpi_name = cell->get_string_attribute(ID(loom_dpi_func));
                if (dpi_name.empty()) {
                    log_warning("  Cell %s has no loom_dpi_func attribute, skipping.\n", log_id(cell));
                    continue;
                }

                log("  Processing DPI call: %s (cell %s)\n", dpi_name.c_str(), log_id(cell));

                DpiFunction func;
                func.name = dpi_name;
                func.func_id = func_id++;
                func.cell = cell;

                // Extract argument and return widths from cell parameters
                if (cell->hasParam(ID(ARG_WIDTH))) {
                    func.arg_width = cell->getParam(ID(ARG_WIDTH)).as_int();
                } else {
                    func.arg_width = 32; // default
                }

                if (cell->hasParam(ID(RET_WIDTH))) {
                    func.ret_width = cell->getParam(ID(RET_WIDTH)).as_int();
                } else {
                    func.ret_width = 32; // default
                }

                // Get return type from attribute
                func.ret_type = cell->get_string_attribute(ID(loom_dpi_ret_type));
                if (func.ret_type.empty()) {
                    func.ret_type = (func.ret_width > 0) ? "int" : "void";
                }

                // Get argument names and types from attributes
                std::string arg_names_str = cell->get_string_attribute(ID(loom_dpi_arg_names));
                std::string arg_types_str = cell->get_string_attribute(ID(loom_dpi_arg_types));
                std::string arg_widths_str = cell->get_string_attribute(ID(loom_dpi_arg_widths));
                std::string arg_dirs_str = cell->get_string_attribute(ID(loom_dpi_arg_dirs));

                // Parse comma-separated argument info
                std::vector<std::string> arg_names = split_string(arg_names_str);
                std::vector<std::string> arg_types = split_string(arg_types_str);
                std::vector<std::string> arg_widths = split_string(arg_widths_str);
                std::vector<std::string> arg_dirs = split_string(arg_dirs_str);

                // Build argument list
                int num_args = (int)arg_names.size();
                if (num_args == 0 && func.arg_width > 0) {
                    // No detailed info, create generic args based on total width
                    num_args = (func.arg_width + 31) / 32;
                    for (int i = 0; i < num_args; i++) {
                        DpiArg arg;
                        arg.name = "arg" + std::to_string(i);
                        arg.type = "int";
                        arg.direction = "input";
                        arg.width = (i == num_args - 1) ?
                            (func.arg_width - i * 32) : 32;
                        func.args.push_back(arg);
                    }
                } else {
                    for (int i = 0; i < num_args; i++) {
                        DpiArg arg;
                        arg.name = arg_names[i];
                        arg.type = (i < (int)arg_types.size()) ? arg_types[i] : "int";
                        arg.direction = (i < (int)arg_dirs.size()) ? arg_dirs[i] : "input";
                        arg.width = (i < (int)arg_widths.size()) ?
                            std::stoi(arg_widths[i]) : 32;
                        func.args.push_back(arg);
                    }
                }

                // Get the argument and result signals
                if (cell->hasPort(ID(ARGS))) {
                    func.args_sig = cell->getPort(ID(ARGS));
                }
                if (cell->hasPort(ID(RESULT))) {
                    func.result_sig = cell->getPort(ID(RESULT));
                }

                // Derive valid condition before removing the cell
                func.valid_condition = derive_valid_condition(module, func);

                module_functions.push_back(func);
                dpi_functions.push_back(func);
            }

            // Second pass: create bridge interface with proper multiplexing
            if (!module_functions.empty()) {
                create_bridge_interface(module, module_functions);
            }

            // Process $__loom_finish cells - transform to hardware output
            process_finish_cells(module);
        }

        if (gen_wrapper && !dpi_functions.empty()) {
            generate_host_wrapper(dpi_functions);
        }

        if (!json_out_path.empty() && !dpi_functions.empty()) {
            write_json_metadata(dpi_functions, json_out_path);
        }

        if (!header_out_path.empty() && !dpi_functions.empty()) {
            write_c_header(dpi_functions, header_out_path);
        }

        log("Processed %zu DPI function(s)\n", dpi_functions.size());
    }

    // Split a comma-separated string into a vector
    std::vector<std::string> split_string(const std::string &s) {
        std::vector<std::string> result;
        if (s.empty()) return result;

        size_t start = 0;
        size_t end = s.find(',');
        while (end != std::string::npos) {
            std::string token = s.substr(start, end - start);
            // Trim whitespace
            size_t first = token.find_first_not_of(" \t");
            size_t last = token.find_last_not_of(" \t");
            if (first != std::string::npos) {
                result.push_back(token.substr(first, last - first + 1));
            }
            start = end + 1;
            end = s.find(',', start);
        }
        std::string token = s.substr(start);
        size_t first = token.find_first_not_of(" \t");
        size_t last = token.find_last_not_of(" \t");
        if (first != std::string::npos) {
            result.push_back(token.substr(first, last - first + 1));
        }
        return result;
    }

    // Derive the execution condition for a DPI call.
    //
    // DPI calls must be in clocked (always_ff) blocks. After proc, the DPI result
    // feeds into FFs through muxes. The mux select signal indicates when the
    // DPI call is active (e.g., state == StCallAdd).
    //
    // This function traces the DPI result signal to find the $pmux select bit
    // that gates when this specific DPI call is executed.
    RTLIL::SigSpec derive_valid_condition(RTLIL::Module *module, const DpiFunction &func) {
        SigMap sigmap(module);

        RTLIL::SigSpec result_sig = sigmap(func.result_sig);
        if (GetSize(result_sig) == 0) {
            log("    No result signal, defaulting to valid=1\n");
            return RTLIL::SigSpec(RTLIL::State::S1);
        }

        log("    Tracing result signal: %s\n", log_signal(result_sig));

        // Find the $pmux that uses this DPI result and extract the specific select bit.
        // DPI calls in clocked blocks produce: DPI_result -> $pmux input -> FF
        for (auto cell : module->cells()) {
            if (cell->type != ID($pmux))
                continue;

            // $pmux has: A (default), B (concatenated cases), S (one-hot select)
            RTLIL::SigSpec port_b = sigmap(cell->getPort(ID::B));
            RTLIL::SigSpec port_s = cell->getPort(ID::S);
            int width = GetSize(cell->getPort(ID::A));
            int n_cases = GetSize(port_s);

            // Check each case input for the DPI result
            for (int case_idx = 0; case_idx < n_cases; case_idx++) {
                RTLIL::SigSpec case_input = sigmap(port_b.extract(case_idx * width, width));

                // Check if any bit of result_sig matches this case input
                bool matches = false;
                for (int i = 0; i < GetSize(case_input) && i < GetSize(result_sig); i++) {
                    if (case_input[i] == result_sig[i]) {
                        matches = true;
                        break;
                    }
                }

                if (matches) {
                    RTLIL::SigBit sel_bit = port_s[case_idx];
                    log("    Found valid condition: %s (case %d of %s)\n",
                        log_signal(sel_bit), case_idx, log_id(cell));
                    return RTLIL::SigSpec(sel_bit);
                }
            }
        }

        // Fallback: check for simple 2:1 $mux (single DPI call case)
        for (auto cell : module->cells()) {
            if (cell->type != ID($mux))
                continue;

            RTLIL::SigSpec port_b = sigmap(cell->getPort(ID::B));
            bool matches = false;
            for (int i = 0; i < GetSize(port_b) && i < GetSize(result_sig); i++) {
                if (port_b[i] == result_sig[i]) {
                    matches = true;
                    break;
                }
            }

            if (matches) {
                RTLIL::SigSpec sel = cell->getPort(ID::S);
                log("    Found valid condition: %s (from $mux %s)\n",
                    log_signal(sel), log_id(cell));
                return sel;
            }
        }

        log_warning("    Could not derive valid condition for DPI call '%s'\n", func.name.c_str());
        log_warning("    DPI calls should only be in clocked (always_ff) blocks\n");
        return RTLIL::SigSpec(RTLIL::State::S1);
    }

    // Process $__loom_finish cells and convert to hardware output signal.
    // The $__loom_finish cell has EN and TRG ports that indicate when $finish is called.
    // We create a loom_finish_o output port and connect the EN signal to it.
    void process_finish_cells(RTLIL::Module *module) {
        std::vector<RTLIL::Cell*> finish_cells;

        for (auto cell : module->cells()) {
            if (cell->type == ID($__loom_finish)) {
                finish_cells.push_back(cell);
            }
        }

        if (finish_cells.empty()) {
            return;
        }

        log("  Found %zu $finish cell(s)\n", finish_cells.size());

        // Create the finish output port
        RTLIL::Wire *finish_out = module->addWire(ID(loom_finish_o), 1);
        finish_out->port_output = true;

        // If multiple $finish cells, OR their enable signals together
        RTLIL::SigSpec combined_en;

        for (auto cell : finish_cells) {
            log("    Processing $finish cell %s\n", log_id(cell));

            // Get the enable signal from the cell
            if (!cell->hasPort(ID::EN)) {
                log_warning("    $__loom_finish cell %s has no EN port, using const 1\n", log_id(cell));
                combined_en.append(RTLIL::State::S1);
            } else {
                RTLIL::SigSpec en_sig = cell->getPort(ID::EN);
                log("      EN signal: %s\n", log_signal(en_sig));
                combined_en.append(en_sig);
            }

            // Log exit code info
            if (cell->hasParam(ID(EXIT_CODE))) {
                int exit_code = cell->getParam(ID(EXIT_CODE)).as_int();
                log("      Exit code: %d\n", exit_code);
            }

            // Remove the cell - it's been transformed to hardware
            module->remove(cell);
        }

        // Connect to output port
        if (GetSize(combined_en) == 1) {
            // Single $finish - direct connection
            module->connect(finish_out, combined_en);
        } else {
            // Multiple $finish cells - OR them together
            RTLIL::SigSpec or_result = combined_en[0];
            for (int i = 1; i < GetSize(combined_en); i++) {
                RTLIL::Wire *or_wire = module->addWire(NEW_ID, 1);
                module->addOr(NEW_ID, or_result, combined_en[i], or_wire);
                or_result = or_wire;
            }
            module->connect(finish_out, or_result);
        }

        module->fixup_ports();
        log("  Created loom_finish_o output port\n");
    }

    // Create bridge interface with proper multiplexing for multiple DPI functions.
    // This is called once per module after all DPI functions are collected.
    void create_bridge_interface(RTLIL::Module *module, std::vector<DpiFunction> &functions) {
        // Calculate maximum widths across all functions
        int max_arg_width = 0;
        int max_ret_width = 0;
        for (const auto &func : functions) {
            max_arg_width = std::max(max_arg_width, func.arg_width);
            max_ret_width = std::max(max_ret_width, func.ret_width);
        }

        // Create bridge interface ports
        RTLIL::Wire *dpi_valid = module->addWire(ID(loom_dpi_valid), 1);
        dpi_valid->port_output = true;

        RTLIL::Wire *dpi_func_id = module->addWire(ID(loom_dpi_func_id), 8);
        dpi_func_id->port_output = true;

        RTLIL::Wire *dpi_args_out = module->addWire(ID(loom_dpi_args), max_arg_width);
        dpi_args_out->port_output = true;

        RTLIL::Wire *dpi_result_in = module->addWire(ID(loom_dpi_result), max_ret_width);
        dpi_result_in->port_input = true;

        // For single function case, no muxing needed
        if (functions.size() == 1) {
            const DpiFunction &func = functions[0];

            // Connect valid
            module->connect(RTLIL::SigSpec(dpi_valid), func.valid_condition);

            // Connect func_id
            module->connect(RTLIL::SigSpec(dpi_func_id), RTLIL::SigSpec(func.func_id, 8));

            // Connect args (pad to max width)
            RTLIL::SigSpec padded_args = func.args_sig;
            if (GetSize(func.args_sig) < max_arg_width) {
                padded_args.append(RTLIL::SigSpec(RTLIL::State::S0,
                    max_arg_width - GetSize(func.args_sig)));
            }
            module->connect(RTLIL::SigSpec(dpi_args_out), padded_args);

            // Connect result to DPI result port
            if (GetSize(func.result_sig) > 0) {
                module->connect(func.result_sig,
                    RTLIL::SigSpec(dpi_result_in).extract(0, GetSize(func.result_sig)));
            }

            // Remove placeholder cell
            module->remove(func.cell);

            log("    Converted to bridge: func_id=%d, arg_width=%d, ret_width=%d\n",
                func.func_id, func.arg_width, func.ret_width);
            log("    Base address: 0x%04x\n", LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN);
        } else {
            // Multiple functions: need to multiplex outputs
            // dpi_valid = valid_0 | valid_1 | ... | valid_n
            // dpi_args = valid_0 ? args_0 : (valid_1 ? args_1 : ... : 0)
            // dpi_func_id = valid_0 ? id_0 : (valid_1 ? id_1 : ... : 0)

            log("  Creating multiplexed bridge for %zu DPI functions\n", functions.size());

            // Helper to reduce multi-bit valid to single bit (OR all bits)
            auto reduce_to_1bit = [&](RTLIL::SigSpec sig) -> RTLIL::SigSpec {
                if (GetSize(sig) == 1)
                    return sig;
                // OR all bits together to get single-bit valid
                RTLIL::Wire *reduced = module->addWire(NEW_ID, 1);
                module->addReduceOr(NEW_ID, sig, reduced);
                log("    Reduced %d-bit valid to 1-bit\n", GetSize(sig));
                return RTLIL::SigSpec(reduced);
            };

            // Reduce all valid conditions to 1 bit
            std::vector<RTLIL::SigSpec> valid_1bit;
            for (auto &func : functions) {
                valid_1bit.push_back(reduce_to_1bit(func.valid_condition));
            }

            // Build OR tree for valid signal
            RTLIL::SigSpec valid_or = valid_1bit[0];
            for (size_t i = 1; i < functions.size(); i++) {
                RTLIL::Wire *or_out = module->addWire(NEW_ID, 1);
                module->addOr(NEW_ID, valid_or, valid_1bit[i], or_out);
                valid_or = RTLIL::SigSpec(or_out);
            }
            module->connect(RTLIL::SigSpec(dpi_valid), valid_or);

            // Build priority mux tree for func_id (from last to first)
            // Start with default value of 0
            RTLIL::SigSpec func_id_mux = RTLIL::SigSpec(RTLIL::State::S0, 8);
            for (int i = (int)functions.size() - 1; i >= 0; i--) {
                RTLIL::Wire *mux_out = module->addWire(NEW_ID, 8);
                RTLIL::SigSpec func_id_const = RTLIL::SigSpec(functions[i].func_id, 8);
                module->addMux(NEW_ID, func_id_mux, func_id_const,
                    valid_1bit[i], mux_out);
                func_id_mux = RTLIL::SigSpec(mux_out);
            }
            module->connect(RTLIL::SigSpec(dpi_func_id), func_id_mux);

            // Build priority mux tree for args (from last to first)
            RTLIL::SigSpec args_mux = RTLIL::SigSpec(RTLIL::State::S0, max_arg_width);
            for (int i = (int)functions.size() - 1; i >= 0; i--) {
                RTLIL::Wire *mux_out = module->addWire(NEW_ID, max_arg_width);
                // Pad this function's args to max width
                RTLIL::SigSpec padded_args = functions[i].args_sig;
                if (GetSize(functions[i].args_sig) < max_arg_width) {
                    padded_args.append(RTLIL::SigSpec(RTLIL::State::S0,
                        max_arg_width - GetSize(functions[i].args_sig)));
                }
                module->addMux(NEW_ID, args_mux, padded_args,
                    valid_1bit[i], mux_out);
                args_mux = RTLIL::SigSpec(mux_out);
            }
            module->connect(RTLIL::SigSpec(dpi_args_out), args_mux);

            // All functions share the same result port - connect each result signal
            // The result is only meaningful when that function's call completes
            for (const auto &func : functions) {
                if (GetSize(func.result_sig) > 0) {
                    module->connect(func.result_sig,
                        RTLIL::SigSpec(dpi_result_in).extract(0, GetSize(func.result_sig)));
                }

                // Remove placeholder cell
                module->remove(func.cell);

                log("    Converted to bridge: func_id=%d, arg_width=%d, ret_width=%d\n",
                    func.func_id, func.arg_width, func.ret_width);
                log("    Base address: 0x%04x\n", LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN);
            }
        }

        module->fixup_ports();
    }

    void generate_host_wrapper(const std::vector<DpiFunction> &functions) {
        log("\nDPI Bridge Interface Summary:\n");
        log("// Clock gating: emu_top gates all clocks when loom_dpi_valid=1\n");
        log("// Mailbox base: 0x%04x\n", LOOM_MAILBOX_BASE);
        log("// DPI base: 0x%04x\n", LOOM_DPI_BASE);
        log("\n");

        for (const auto &func : functions) {
            int addr = LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN;
            log("// Function: %s (ID: %d)\n", func.name.c_str(), func.func_id);
            log("//   Base address:    0x%04x\n", addr);
            log("//   Status register: 0x%04x\n", addr);
            log("//   Arg registers:   0x%04x (%d bits total)\n", addr + 0x04, func.arg_width);
            log("//   Ret registers:   0x%04x (%d bits)\n",
                addr + 0x04 + ((func.arg_width + 31) / 32) * 4, func.ret_width);
            log("\n");
        }
    }

    void write_json_metadata(const std::vector<DpiFunction> &functions, const std::string &path) {
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            log_error("Cannot open JSON output file: %s\n", path.c_str());
            return;
        }

        ofs << "{\n";
        ofs << "  \"mailbox_base\": \"0x" << std::hex << LOOM_MAILBOX_BASE << "\",\n";
        ofs << "  \"dpi_base\": \"0x" << LOOM_DPI_BASE << "\",\n";
        ofs << "  \"func_block_size\": " << std::dec << FUNC_BLOCK_ALIGN << ",\n";
        ofs << "  \"dpi_functions\": [\n";

        for (size_t i = 0; i < functions.size(); i++) {
            const auto &func = functions[i];
            int base_addr = LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN;

            ofs << "    {\n";
            ofs << "      \"id\": " << func.func_id << ",\n";
            ofs << "      \"name\": \"" << func.name << "\",\n";
            ofs << "      \"base_addr\": \"0x" << std::hex << base_addr << std::dec << "\",\n";

            // Return type
            if (func.ret_width > 0) {
                ofs << "      \"return\": {\n";
                ofs << "        \"type\": \"" << func.ret_type << "\",\n";
                ofs << "        \"width\": " << func.ret_width << "\n";
                ofs << "      },\n";
            } else {
                ofs << "      \"return\": null,\n";
            }

            // Arguments
            ofs << "      \"args\": [\n";
            for (size_t j = 0; j < func.args.size(); j++) {
                const auto &arg = func.args[j];
                ofs << "        {\n";
                ofs << "          \"name\": \"" << arg.name << "\",\n";
                ofs << "          \"direction\": \"" << arg.direction << "\",\n";
                ofs << "          \"type\": \"" << arg.type << "\",\n";
                ofs << "          \"width\": " << arg.width << "\n";
                ofs << "        }";
                if (j + 1 < func.args.size()) ofs << ",";
                ofs << "\n";
            }
            ofs << "      ]\n";

            ofs << "    }";
            if (i + 1 < functions.size()) ofs << ",";
            ofs << "\n";
        }

        ofs << "  ]\n";
        ofs << "}\n";

        ofs.close();
        log("Wrote DPI metadata to: %s\n", path.c_str());
    }

    // Map SV/DPI type to C type
    std::string sv_type_to_c(const std::string &sv_type, int width) {
        // Standard DPI type mappings
        if (sv_type == "int" || sv_type == "integer") {
            return "int32_t";
        }
        if (sv_type == "shortint") {
            return "int16_t";
        }
        if (sv_type == "longint") {
            return "int64_t";
        }
        if (sv_type == "byte") {
            return "int8_t";
        }
        if (sv_type == "bit" || sv_type == "logic" || sv_type == "reg") {
            if (width <= 8) return "uint8_t";
            if (width <= 16) return "uint16_t";
            if (width <= 32) return "uint32_t";
            return "uint64_t";
        }
        if (sv_type == "string") {
            return "const char*";
        }
        if (sv_type == "void") {
            return "void";
        }
        // Default to uint32_t for unknown types
        if (width <= 32) return "uint32_t";
        return "uint64_t";
    }

    void write_c_header(const std::vector<DpiFunction> &functions, const std::string &path) {
        std::ofstream ofs(path);
        if (!ofs.is_open()) {
            log_error("Cannot open C source output file: %s\n", path.c_str());
            return;
        }

        ofs << "// SPDX-License-Identifier: Apache-2.0\n";
        ofs << "// Generated by Loom dpi_bridge pass - DO NOT EDIT\n";
        ofs << "//\n";
        ofs << "// DPI function dispatch table and wrappers.\n";
        ofs << "// Link this with your DPI implementation.\n";
        ofs << "\n";
        ofs << "#include <stdint.h>\n";
        ofs << "#include <loom_dpi_service.h>\n";
        ofs << "\n";

        // Function prototypes (extern declarations for user-provided functions)
        ofs << "// User-provided DPI function implementations\n";
        for (const auto &func : functions) {
            std::string ret_type = sv_type_to_c(func.ret_type, func.ret_width);
            if (func.ret_width == 0) {
                ret_type = "void";
            }

            ofs << "extern " << ret_type << " " << func.name << "(";

            if (func.args.empty()) {
                ofs << "void";
            } else {
                for (size_t i = 0; i < func.args.size(); i++) {
                    const auto &arg = func.args[i];
                    std::string c_type = sv_type_to_c(arg.type, arg.width);
                    if (i > 0) ofs << ", ";
                    ofs << c_type << " " << arg.name;
                }
            }
            ofs << ");\n";
        }
        ofs << "\n";

        // Generate wrapper functions for uniform callback interface
        ofs << "// Wrapper functions for uniform callback interface\n";
        for (const auto &func : functions) {
            ofs << "static uint64_t _loom_wrap_" << func.name << "(const uint32_t *args) {\n";

            // Build argument list with proper type casting
            std::string call_args;
            int arg_offset = 0;
            for (size_t i = 0; i < func.args.size(); i++) {
                const auto &arg = func.args[i];
                std::string c_type = sv_type_to_c(arg.type, arg.width);
                if (i > 0) call_args += ", ";
                call_args += "(" + c_type + ")args[" + std::to_string(arg_offset) + "]";
                arg_offset += (arg.width + 31) / 32;
            }

            if (func.ret_width > 0) {
                ofs << "    return (uint64_t)" << func.name << "(" << call_args << ");\n";
            } else {
                ofs << "    " << func.name << "(" << call_args << ");\n";
                ofs << "    return 0;\n";
            }
            ofs << "}\n\n";
        }

        // Generate the function table
        ofs << "// DPI function table for loom_sim_main\n";
        ofs << "const loom_dpi_func_t loom_dpi_funcs[] = {\n";
        for (size_t i = 0; i < functions.size(); i++) {
            const auto &func = functions[i];
            ofs << "    { " << func.func_id << ", \"" << func.name << "\", "
                << func.args.size() << ", " << func.ret_width << ", _loom_wrap_" << func.name << " }";
            if (i + 1 < functions.size()) ofs << ",";
            ofs << "\n";
        }
        ofs << "};\n\n";

        ofs << "const int loom_dpi_n_funcs = " << functions.size() << ";\n";

        ofs.close();
        log("Wrote C source to: %s\n", path.c_str());
    }
};

DpiBridgePass DpiBridgePass_singleton;

PRIVATE_NAMESPACE_END
