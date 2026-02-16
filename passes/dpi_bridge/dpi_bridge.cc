// SPDX-License-Identifier: Apache-2.0
/*
 * dpi_bridge - Yosys pass for DPI-C function bridge generation
 *
 * This pass converts DPI function call cells ($__loom_dpi_call) into hardware
 * interfaces that enable FPGA<->host communication for emulation.
 *
 * DPI functions cannot be synthesized directly. The yosys-slang frontend
 * automatically creates $__loom_dpi_call cells for DPI import calls, which
 * this pass then converts to hardware bridges.
 *
 * DUT bridge interface (directly connects to emu_top wrapper):
 *   - loom_dpi_valid:   DPI call pending (output, triggers clock gating)
 *   - loom_dpi_func_id: Function identifier (output)
 *   - loom_dpi_args:    Packed function arguments (output)
 *   - loom_dpi_result:  Return value from host (input)
 *
 * Note: loom_dpi_ack is NOT part of the DUT interface. It's added by emu_top
 * for clock gating control. The DUT is clock-gated and transparent to the
 * handshaking - it just sees the result when the clock resumes.
 *
 * The pass also outputs JSON metadata that can be consumed by
 * scripts/loom_dpi_codegen.py to generate host-side dispatch code.
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
    RTLIL::SigSpec valid_condition;  // Derived execution condition
};

struct DpiBridgePass : public Pass {
    DpiBridgePass() : Pass("dpi_bridge", "Convert DPI placeholders to hardware bridges") {}

    void help() override {
        log("\n");
        log("    dpi_bridge [options] [selection]\n");
        log("\n");
        log("Convert DPI function call cells to hardware bridge interfaces.\n");
        log("\n");
        log("This pass processes $__loom_dpi_call cells created by yosys-slang\n");
        log("when it encounters DPI import function calls in SystemVerilog.\n");
        log("\n");
        log("    -json_out <file>\n");
        log("        Write DPI metadata to JSON file for host code generation.\n");
        log("        This file is consumed by scripts/loom_dpi_codegen.py.\n");
        log("\n");
        log("    -gen_wrapper\n");
        log("        Print DPI interface summary to log (legacy)\n");
        log("\n");
        log("The $__loom_dpi_call cells have:\n");
        log("  - Attribute 'loom_dpi_func': DPI function name\n");
        log("  - Attribute 'loom_dpi_ret_type': Return type (optional)\n");
        log("  - Attribute 'loom_dpi_arg_names': Comma-separated arg names (optional)\n");
        log("  - Attribute 'loom_dpi_arg_types': Comma-separated arg types (optional)\n");
        log("  - Parameter ARG_WIDTH: Total width of packed arguments\n");
        log("  - Parameter RET_WIDTH: Width of return value\n");
        log("  - Port ARGS: Packed function arguments\n");
        log("  - Port RESULT: Return value wire\n");
        log("\n");
        log("DUT ports created:\n");
        log("  - loom_dpi_valid:   DPI call pending (output, gates clock at emu_top)\n");
        log("  - loom_dpi_func_id: Function identifier (output, 8-bit)\n");
        log("  - loom_dpi_args:    Packed function arguments (output)\n");
        log("  - loom_dpi_result:  Return value from host (input)\n");
        log("\n");
        log("Note: loom_dpi_ack is added by emu_top wrapper, not the DUT.\n");
        log("The DUT is clock-gated and transparent to the handshaking.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing DPI_BRIDGE pass.\n");

        bool gen_wrapper = false;
        std::string json_out_path;

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
            break;
        }
        extra_args(args, argidx, design);

        int func_id = 0;
        std::vector<DpiFunction> dpi_functions;

        for (auto module : design->selected_modules()) {
            log("Processing module %s\n", log_id(module));

            std::vector<RTLIL::Cell*> cells_to_process;

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

                dpi_functions.push_back(func);

                // Convert DPI call to bridge interface
                convert_to_bridge(module, func);
            }
        }

        if (gen_wrapper && !dpi_functions.empty()) {
            generate_host_wrapper(dpi_functions);
        }

        if (!json_out_path.empty() && !dpi_functions.empty()) {
            write_json_metadata(dpi_functions, json_out_path);
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

    // Derive the execution condition for a DPI call by finding how the result is used.
    //
    // Strategy: Look for flip-flops (adffe, dffe, etc.) that capture the DPI result.
    // The enable condition of such FFs represents when the DPI call is active.
    //
    // After proc, the design will have FFs like:
    //   always @(posedge clk) if (enable) result_q <= dpi_result;
    // The 'enable' signal is what we want as the valid condition.
    RTLIL::SigSpec derive_valid_condition(RTLIL::Module *module, const DpiFunction &func) {
        SigMap sigmap(module);

        // Get the DPI result signal
        RTLIL::SigSpec result_sig = sigmap(func.result_sig);
        if (GetSize(result_sig) == 0) {
            log("    No result signal, defaulting to valid=1\n");
            return RTLIL::SigSpec(RTLIL::State::S1);
        }

        // Look for FFs (with enable) that use the DPI result as their D input
        for (auto cell : module->cells()) {
            // Check for various FF types with enable
            if (!cell->type.in(ID($dffe), ID($adffe), ID($sdffe), ID($sdffce), ID($dffsre), ID($aldffe)))
                continue;

            RTLIL::SigSpec ff_d = sigmap(cell->getPort(ID::D));

            // Check if FF's D input matches the DPI result
            bool matches = false;
            for (int i = 0; i < GetSize(ff_d) && i < GetSize(result_sig); i++) {
                if (ff_d[i] == result_sig[i]) {
                    matches = true;
                    break;
                }
            }

            if (matches && cell->hasPort(ID::EN)) {
                RTLIL::SigSpec enable = cell->getPort(ID::EN);
                log("    Found FF with enable capturing DPI result: %s, EN=%s\n",
                    log_id(cell), log_signal(enable));
                return enable;
            }
        }

        // Fallback: Look for muxes that select the DPI result vs feedback
        // This handles patterns like: result_d = enable ? dpi_result : result_q;
        for (auto cell : module->cells()) {
            if (cell->type != ID($mux))
                continue;

            RTLIL::SigSpec port_a = sigmap(cell->getPort(ID::A));
            RTLIL::SigSpec port_b = sigmap(cell->getPort(ID::B));

            // Check if port B is the DPI result (selected when S=1)
            bool b_matches = false;
            for (int i = 0; i < GetSize(port_b) && i < GetSize(result_sig); i++) {
                if (port_b[i] == result_sig[i]) {
                    b_matches = true;
                    break;
                }
            }

            if (b_matches) {
                RTLIL::SigSpec sel = cell->getPort(ID::S);
                log("    Found mux selecting DPI result: %s, select=%s\n",
                    log_id(cell), log_signal(sel));
                return sel;
            }
        }

        log("    No enable/select condition found, defaulting to valid=1\n");
        return RTLIL::SigSpec(RTLIL::State::S1);
    }

    void convert_to_bridge(RTLIL::Module *module, const DpiFunction &func) {
        RTLIL::Cell *cell = func.cell;

        // Create bridge interface ports if they don't exist
        // Use loom_ prefix to avoid name conflicts with user signals

        // loom_dpi_valid: indicates a DPI call is pending
        // This signal triggers clock gating at the emu_top level
        RTLIL::Wire *dpi_valid = module->wire(ID(loom_dpi_valid));
        if (!dpi_valid) {
            dpi_valid = module->addWire(ID(loom_dpi_valid), 1);
            dpi_valid->port_output = true;
        }

        // Note: loom_dpi_ack is NOT added to the DUT - it's only used by emu_top
        // for clock gating. The DUT is transparent to the handshaking since
        // its clock is gated while waiting for the host response.

        // loom_dpi_func_id: identifies which DPI function is being called
        RTLIL::Wire *dpi_func_id = module->wire(ID(loom_dpi_func_id));
        if (!dpi_func_id) {
            dpi_func_id = module->addWire(ID(loom_dpi_func_id), 8);
            dpi_func_id->port_output = true;
        }

        // Create or extend args output port
        RTLIL::Wire *dpi_args_out = module->wire(ID(loom_dpi_args));
        if (!dpi_args_out) {
            dpi_args_out = module->addWire(ID(loom_dpi_args), func.arg_width);
            dpi_args_out->port_output = true;
        } else if (GetSize(dpi_args_out) < func.arg_width) {
            // Extend width if needed for this function
            dpi_args_out->width = func.arg_width;
        }

        // Create or extend result input port
        RTLIL::Wire *dpi_result_in = module->wire(ID(loom_dpi_result));
        if (!dpi_result_in) {
            dpi_result_in = module->addWire(ID(loom_dpi_result), func.ret_width);
            dpi_result_in->port_input = true;
        } else if (GetSize(dpi_result_in) < func.ret_width) {
            dpi_result_in->width = func.ret_width;
        }

        // Derive the valid condition from the DPI call's execution context
        // This traces how the result is used through mux chains to find when
        // the DPI result is actually needed (e.g., state==IDLE && start)
        RTLIL::SigSpec valid_condition = derive_valid_condition(module, func);

        // Connect dpi_valid to the derived condition
        // The clock gating logic at emu_top uses: CE = !dpi_valid | dpi_ack
        // This means: clock runs when no DPI call OR when host has acked
        module->connect(RTLIL::SigSpec(dpi_valid), valid_condition);

        // Connect args to dpi_args port
        if (GetSize(func.args_sig) > 0) {
            if (GetSize(func.args_sig) <= GetSize(dpi_args_out)) {
                RTLIL::SigSpec padded_args = func.args_sig;
                if (GetSize(func.args_sig) < GetSize(dpi_args_out)) {
                    padded_args.append(RTLIL::SigSpec(RTLIL::State::S0,
                        GetSize(dpi_args_out) - GetSize(func.args_sig)));
                }
                module->connect(RTLIL::SigSpec(dpi_args_out), padded_args);
            } else {
                module->connect(RTLIL::SigSpec(dpi_args_out),
                    func.args_sig.extract(0, GetSize(dpi_args_out)));
            }
        }

        // Connect dpi_result to the result signal
        if (GetSize(func.result_sig) > 0) {
            if (GetSize(func.result_sig) <= GetSize(dpi_result_in)) {
                module->connect(func.result_sig,
                    RTLIL::SigSpec(dpi_result_in).extract(0, GetSize(func.result_sig)));
            } else {
                RTLIL::SigSpec padded_result = RTLIL::SigSpec(dpi_result_in);
                padded_result.append(RTLIL::SigSpec(RTLIL::State::S0,
                    GetSize(func.result_sig) - GetSize(dpi_result_in)));
                module->connect(func.result_sig, padded_result);
            }
        }

        // Connect function ID constant
        module->connect(RTLIL::SigSpec(dpi_func_id), RTLIL::SigSpec(func.func_id, 8));

        // Mark the cell as bridged and remove it
        cell->set_string_attribute(ID(loom_dpi_bridged), func.name);

        // Remove the placeholder cell since we've created the bridge
        module->remove(cell);

        module->fixup_ports();

        int base_addr = LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN;
        log("    Converted to bridge: func_id=%d, arg_width=%d, ret_width=%d\n",
            func.func_id, func.arg_width, func.ret_width);
        log("    Base address: 0x%04x\n", base_addr);
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
};

DpiBridgePass DpiBridgePass_singleton;

PRIVATE_NAMESPACE_END
