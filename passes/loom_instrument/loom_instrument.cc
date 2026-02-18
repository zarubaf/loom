// SPDX-License-Identifier: Apache-2.0
/*
 * loom_instrument - Yosys pass for DUT instrumentation
 *
 * This pass instruments the DUT for Loom emulation:
 *   1. Converts DPI function call cells ($__loom_dpi_call) into hardware
 *      bridge interfaces for FPGA<->host communication.
 *   2. Transforms $__loom_finish cells into hardware output signals.
 *   3. Adds flip-flop enable (loom_en) so the DUT can be frozen while
 *      the clock runs free.  scan_enable overrides loom_en so scanning
 *      always works.
 *
 * IMPORTANT: DPI calls must only appear in clocked (always_ff) blocks.
 *
 * DUT bridge interface (connects to emu_top wrapper):
 *   - loom_dpi_valid:   DPI call pending (output)
 *   - loom_dpi_func_id: Function identifier (output)
 *   - loom_dpi_args:    Packed function arguments (output)
 *   - loom_dpi_result:  Return value from host (input)
 *   - loom_en:          Flip-flop enable (input)
 *
 * The pass also outputs:
 *   - JSON metadata for host-side integration
 *   - C header with function prototypes for user implementation
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"
#include "kernel/fmt.h"
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
    std::string type;         // SV type name
    std::string direction;    // input, output, inout
    int width;
    std::string string_value; // For string args: the compile-time constant value
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
    bool builtin = false;            // true for loom-provided functions (__loom_display_*)
};

struct LoomInstrumentPass : public Pass {
    LoomInstrumentPass() : Pass("loom_instrument", "Instrument DUT for Loom emulation (DPI bridge + flop enable)") {}

    void help() override {
        log("\n");
        log("    loom_instrument [options] [selection]\n");
        log("\n");
        log("Instrument DUT for Loom emulation.\n");
        log("\n");
        log("This pass performs:\n");
        log("  1. DPI bridge: convert $__loom_dpi_call cells to hardware interfaces\n");
        log("  2. $finish transform: convert $__loom_finish cells to output ports\n");
        log("  3. Flop enable: add loom_en input that freezes all FFs\n");
        log("\n");
        log("IMPORTANT: DPI calls must only appear in clocked (always_ff) blocks.\n");
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
        log("  - loom_en:          FF enable (input, freezes DUT when low)\n");
        log("  - loom_dpi_valid:   DPI call pending (output)\n");
        log("  - loom_dpi_func_id: Function identifier (output, 8-bit)\n");
        log("  - loom_dpi_args:    Packed function arguments (output)\n");
        log("  - loom_dpi_result:  Return value from host (input)\n");
        log("\n");
    }

    // Check if a cell is a flip-flop type
    static bool is_ff(RTLIL::Cell *cell) {
        return cell->type.in(
            ID($dff), ID($dffe), ID($adff), ID($adffe),
            ID($sdff), ID($sdffe), ID($sdffce),
            ID($dffsr), ID($dffsre), ID($aldff), ID($aldffe)
        );
    }

    // Check if a FF is a memory output register merged by memory_dff.
    static bool is_memory_output_ff(RTLIL::Cell *cell) {
        if (!cell->hasPort(ID::Q))
            return false;

        RTLIL::SigSpec q = cell->getPort(ID::Q);
        for (auto bit : q) {
            if (!bit.wire)
                continue;
            std::string wire_name = bit.wire->name.str();
            if (wire_name.find("ffmerge_disconnected") != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    // Check if a FF already has an enable port
    static bool has_enable(RTLIL::Cell *cell) {
        return cell->type.in(
            ID($dffe), ID($adffe), ID($sdffe), ID($sdffce),
            ID($dffsre), ID($aldffe)
        );
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing LOOM_INSTRUMENT pass.\n");

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

            // Transform $print cells into $__loom_dpi_call cells first
            process_print_cells(module);

            std::vector<RTLIL::Cell*> cells_to_process;
            std::vector<DpiFunction> module_functions;

            // Find $__loom_dpi_call cells (from yosys-slang + converted $print)
            for (auto cell : module->cells()) {
                if (cell->type == ID($__loom_dpi_call)) {
                    cells_to_process.push_back(cell);
                }
            }

            if (cells_to_process.empty()) {
                log("  No DPI call cells found.\n");
            } else {
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
                            // Check for string constant value
                            std::string str_attr = cell->get_string_attribute(
                                RTLIL::IdString("\\loom_dpi_string_arg_" + std::to_string(i)));
                            if (!str_attr.empty() || arg.type == "string") {
                                arg.string_value = str_attr;
                            }
                            func.args.push_back(arg);
                        }
                    }

                    // Check if this is a loom-internal builtin
                    func.builtin = cell->get_bool_attribute(RTLIL::IdString("\\loom_dpi_builtin"));

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

                // Stamp DPI function count so emu_top can read it
                module->set_string_attribute(ID(loom_n_dpi_funcs),
                    std::to_string(module_functions.size()));
            }

            // Process $__loom_finish cells - transform to hardware output
            process_finish_cells(module);

            // Add flop enable logic (must run after DPI bridge and finish processing)
            run_flop_enable(module);
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

    // =========================================================================
    // Flop Enable Logic
    // =========================================================================
    //
    // Adds a loom_en input port and instruments every FF so that:
    //   - loom_en=0, scan_enable=0 → FF frozen
    //   - loom_en=1, scan_enable=0 → normal operation (original EN honored)
    //   - scan_enable=1            → FF always enabled (scan shift override)

    void run_flop_enable(RTLIL::Module *module) {
        // Collect FFs, skipping memory output registers
        std::vector<RTLIL::Cell*> dffs;
        for (auto cell : module->cells()) {
            if (is_ff(cell) && !is_memory_output_ff(cell)) {
                dffs.push_back(cell);
            }
        }

        if (dffs.empty()) {
            log("  No flip-flops found for flop enable.\n");
            return;
        }

        log("  Instrumenting %zu FF(s) with loom_en\n", dffs.size());

        // Add loom_en input port
        RTLIL::Wire *loom_en = module->addWire(ID(loom_en), 1);
        loom_en->port_input = true;

        // Find existing loom_scan_enable wire (from prior scan_insert pass)
        RTLIL::Wire *scan_enable = module->wire(ID(loom_scan_enable));

        // Build combined_en = loom_en | loom_scan_enable
        RTLIL::SigSpec combined_en_1bit;
        if (scan_enable) {
            RTLIL::Wire *comb_wire = module->addWire(NEW_ID, 1);
            module->addOr(NEW_ID, RTLIL::SigSpec(loom_en), RTLIL::SigSpec(scan_enable), RTLIL::SigSpec(comb_wire));
            combined_en_1bit = RTLIL::SigSpec(comb_wire);
            log("  Combined enable: loom_en | loom_scan_enable\n");
        } else {
            combined_en_1bit = RTLIL::SigSpec(loom_en);
            log("  No scan_enable found, using loom_en alone\n");
        }

        for (auto cell : dffs) {
            if (!has_enable(cell)) {
                // Non-enable FF: convert to enable variant
                // $dff → $dffe, $adff → $adffe, etc.
                // EN port is always 1-bit in Yosys FF cells
                if (cell->type == ID($dff)) {
                    cell->type = ID($dffe);
                } else if (cell->type == ID($adff)) {
                    cell->type = ID($adffe);
                } else if (cell->type == ID($sdff)) {
                    cell->type = ID($sdffe);
                } else if (cell->type == ID($dffsr)) {
                    cell->type = ID($dffsre);
                } else if (cell->type == ID($aldff)) {
                    cell->type = ID($aldffe);
                }
                cell->setPort(ID::EN, combined_en_1bit);
                cell->setParam(ID::EN_POLARITY, RTLIL::Const(1, 1));
            } else {
                // Already has enable — combine with loom_en + scan override
                // EN port is always 1-bit in Yosys FF cells
                RTLIL::IdString en_port = ID::EN;
                RTLIL::IdString pol_param = ID::EN_POLARITY;

                RTLIL::SigSpec orig_en = cell->getPort(en_port);
                int polarity = cell->getParam(pol_param).as_int();

                // Compute active_en: if polarity==1, active_en = orig_EN; else ~orig_EN
                RTLIL::SigSpec active_en;
                if (polarity == 1) {
                    active_en = orig_en;
                } else {
                    RTLIL::Wire *inv_wire = module->addWire(NEW_ID, 1);
                    module->addNot(NEW_ID, orig_en, RTLIL::SigSpec(inv_wire));
                    active_en = RTLIL::SigSpec(inv_wire);
                }

                // new_EN = (active_en & loom_en) | scan_enable
                RTLIL::Wire *gated_wire = module->addWire(NEW_ID, 1);
                module->addAnd(NEW_ID, active_en, RTLIL::SigSpec(loom_en), RTLIL::SigSpec(gated_wire));

                RTLIL::SigSpec new_en;
                if (scan_enable) {
                    RTLIL::Wire *final_wire = module->addWire(NEW_ID, 1);
                    module->addOr(NEW_ID, RTLIL::SigSpec(gated_wire), RTLIL::SigSpec(scan_enable), RTLIL::SigSpec(final_wire));
                    new_en = RTLIL::SigSpec(final_wire);
                } else {
                    new_en = RTLIL::SigSpec(gated_wire);
                }

                cell->setPort(en_port, new_en);
                cell->setParam(pol_param, RTLIL::Const(1, 1));
            }
        }

        module->fixup_ports();
        log("  Added loom_en port, instrumented %zu FF(s)\n", dffs.size());
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
    // Prefers the EN port (set by yosys-slang's set_effects_trigger) if available.
    // Falls back to tracing the RESULT signal through pmux/mux select bits.
    RTLIL::SigSpec derive_valid_condition(RTLIL::Module *module, const DpiFunction &func) {
        SigMap sigmap(module);

        // If the cell has an EN port (set by yosys-slang for procedural calls),
        // use it directly as the valid condition.
        if (func.cell->hasPort(ID::EN)) {
            RTLIL::SigSpec en = func.cell->getPort(ID::EN);
            if (GetSize(en) > 0) {
                log("    Using EN port as valid condition: %s\n", log_signal(en));
                return en;
            }
        }

        RTLIL::SigSpec result_sig = sigmap(func.result_sig);
        if (GetSize(result_sig) == 0) {
            log_warning("    No result signal and no EN port, defaulting to valid=1\n");
            return RTLIL::SigSpec(RTLIL::State::S1);
        }

        log("    Tracing result signal: %s\n", log_signal(result_sig));

        // Find the $pmux that uses this DPI result and extract the specific select bit.
        for (auto cell : module->cells()) {
            if (cell->type != ID($pmux))
                continue;

            RTLIL::SigSpec port_b = sigmap(cell->getPort(ID::B));
            RTLIL::SigSpec port_s = cell->getPort(ID::S);
            int width = GetSize(cell->getPort(ID::A));
            int n_cases = GetSize(port_s);

            for (int case_idx = 0; case_idx < n_cases; case_idx++) {
                RTLIL::SigSpec case_input = sigmap(port_b.extract(case_idx * width, width));

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
    // =========================================================================
    // $print → $__loom_dpi_call transformation
    // =========================================================================
    //
    // Converts $print cells (from $display/$write) into DPI calls.
    // The format string is stored as a string attribute (compile-time constant,
    // not in the hardware args bus).  Only the variable signal arguments travel
    // through the DPI bridge registers.
    //
    // The generated C wrapper reconstructs printf() from the format string and
    // the register values — no user implementation needed.

    void process_print_cells(RTLIL::Module *module) {
        static int display_counter = 0;
        std::vector<RTLIL::Cell*> print_cells;

        for (auto cell : module->cells()) {
            if (cell->type == ID($print)) {
                print_cells.push_back(cell);
            }
        }

        if (print_cells.empty())
            return;

        log("  Found %zu $display/$print cell(s)\n", print_cells.size());

        for (auto cell : print_cells) {
            // Parse the Yosys Fmt structure from the $print cell
            Fmt fmt;
            fmt.parse_rtlil(cell);

            // Build the C printf format string and collect signal arguments
            std::string c_fmt;
            std::string arg_names, arg_types, arg_widths, arg_dirs;
            RTLIL::SigSpec hw_args;
            int hw_arg_idx = 0;

            // First arg is the format string (string type, width 0)
            arg_names = "fmt";
            arg_types = "string";
            arg_widths = "0";
            arg_dirs = "input";

            for (const auto &part : fmt.parts) {
                if (part.type == FmtPart::LITERAL) {
                    // Escape for C string
                    for (char ch : part.str) {
                        if (ch == '\\') c_fmt += "\\\\";
                        else if (ch == '"') c_fmt += "\\\"";
                        else if (ch == '\n') c_fmt += "\\n";
                        else if (ch == '\t') c_fmt += "\\t";
                        else c_fmt += ch;
                    }
                } else if (part.type == FmtPart::INTEGER) {
                    int sig_width = GetSize(part.sig);

                    // Map Yosys format spec to printf conversion
                    if (part.base == 16) {
                        c_fmt += part.hex_upper ? "%X" : "%x";
                    } else if (part.base == 10 && !part.signed_) {
                        c_fmt += "%u";
                    } else if (part.base == 10) {
                        c_fmt += "%d";
                    } else if (part.base == 8) {
                        c_fmt += "%o";
                    } else if (part.base == 2) {
                        // Binary: no direct printf, use hex as fallback
                        c_fmt += "0x%x";
                    } else {
                        c_fmt += "%d";
                    }

                    hw_args.append(part.sig);

                    if (hw_arg_idx > 0 || !arg_names.empty()) {
                        arg_names += ",";
                        arg_types += ",";
                        arg_widths += ",";
                        arg_dirs += ",";
                    }
                    arg_names += "arg" + std::to_string(hw_arg_idx);
                    arg_types += part.signed_ ? "int" : "bit";
                    arg_widths += std::to_string(sig_width);
                    arg_dirs += "input";
                    hw_arg_idx++;
                } else if (part.type == FmtPart::STRING) {
                    c_fmt += "%s";
                    hw_args.append(part.sig);

                    if (!arg_names.empty()) {
                        arg_names += ",";
                        arg_types += ",";
                        arg_widths += ",";
                        arg_dirs += ",";
                    }
                    arg_names += "arg" + std::to_string(hw_arg_idx);
                    arg_types += "bit";
                    arg_widths += std::to_string(GetSize(part.sig));
                    arg_dirs += "input";
                    hw_arg_idx++;
                }
                // UNICHAR and VLOG_TIME: skip for now
            }

            int total_hw_width = GetSize(hw_args);

            // Create a $__loom_dpi_call cell to replace the $print
            std::string dpi_name = "__loom_display_" + std::to_string(display_counter++);
            RTLIL::Cell *dpi_cell = module->addCell(NEW_ID, ID($__loom_dpi_call));
            dpi_cell->set_string_attribute(ID(loom_dpi_func), dpi_name);
            dpi_cell->set_bool_attribute(ID::blackbox, true);
            dpi_cell->setPort(ID(ARGS), hw_args);
            dpi_cell->setParam(ID(ARG_WIDTH), total_hw_width);
            dpi_cell->setParam(ID(RET_WIDTH), 0);
            dpi_cell->setParam(ID(NUM_ARGS), hw_arg_idx + 1); // +1 for format string

            // Empty result port (void)
            dpi_cell->setPort(ID(RESULT), RTLIL::SigSpec());

            // Store argument metadata
            dpi_cell->set_string_attribute(ID(loom_dpi_arg_names), arg_names);
            dpi_cell->set_string_attribute(ID(loom_dpi_arg_types), arg_types);
            dpi_cell->set_string_attribute(ID(loom_dpi_arg_widths), arg_widths);
            dpi_cell->set_string_attribute(ID(loom_dpi_arg_dirs), arg_dirs);
            dpi_cell->set_string_attribute(ID(loom_dpi_ret_type), "void");

            // Store format string as string arg attribute
            dpi_cell->set_string_attribute(
                RTLIL::IdString("\\loom_dpi_string_arg_0"), c_fmt);

            // Mark as loom-internal display function
            dpi_cell->set_bool_attribute(RTLIL::IdString("\\loom_dpi_builtin"), true);

            // Propagate EN from the $print cell so derive_valid_condition can use it
            if (cell->hasPort(ID::EN)) {
                dpi_cell->setPort(ID::EN, cell->getPort(ID::EN));
            }

            log("    Converted $print → %s (fmt=\"%s\", %d hw args, %d bits)\n",
                dpi_name.c_str(), c_fmt.c_str(), hw_arg_idx, total_hw_width);

            // Remove the original $print cell
            module->remove(cell);
        }
    }

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
            module->connect(finish_out, combined_en);
        } else {
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

            module->connect(RTLIL::SigSpec(dpi_valid), func.valid_condition);
            module->connect(RTLIL::SigSpec(dpi_func_id), RTLIL::SigSpec(func.func_id, 8));

            RTLIL::SigSpec padded_args = func.args_sig;
            if (GetSize(func.args_sig) < max_arg_width) {
                padded_args.append(RTLIL::SigSpec(RTLIL::State::S0,
                    max_arg_width - GetSize(func.args_sig)));
            }
            module->connect(RTLIL::SigSpec(dpi_args_out), padded_args);

            if (GetSize(func.result_sig) > 0) {
                module->connect(func.result_sig,
                    RTLIL::SigSpec(dpi_result_in).extract(0, GetSize(func.result_sig)));
            }

            module->remove(func.cell);

            log("    Converted to bridge: func_id=%d, arg_width=%d, ret_width=%d\n",
                func.func_id, func.arg_width, func.ret_width);
            log("    Base address: 0x%04x\n", LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN);
        } else {
            log("  Creating multiplexed bridge for %zu DPI functions\n", functions.size());

            auto reduce_to_1bit = [&](RTLIL::SigSpec sig) -> RTLIL::SigSpec {
                if (GetSize(sig) == 1)
                    return sig;
                RTLIL::Wire *reduced = module->addWire(NEW_ID, 1);
                module->addReduceOr(NEW_ID, sig, reduced);
                log("    Reduced %d-bit valid to 1-bit\n", GetSize(sig));
                return RTLIL::SigSpec(reduced);
            };

            std::vector<RTLIL::SigSpec> valid_1bit;
            for (auto &func : functions) {
                valid_1bit.push_back(reduce_to_1bit(func.valid_condition));
            }

            RTLIL::SigSpec valid_or = valid_1bit[0];
            for (size_t i = 1; i < functions.size(); i++) {
                RTLIL::Wire *or_out = module->addWire(NEW_ID, 1);
                module->addOr(NEW_ID, valid_or, valid_1bit[i], or_out);
                valid_or = RTLIL::SigSpec(or_out);
            }
            module->connect(RTLIL::SigSpec(dpi_valid), valid_or);

            RTLIL::SigSpec func_id_mux = RTLIL::SigSpec(RTLIL::State::S0, 8);
            for (int i = (int)functions.size() - 1; i >= 0; i--) {
                RTLIL::Wire *mux_out = module->addWire(NEW_ID, 8);
                RTLIL::SigSpec func_id_const = RTLIL::SigSpec(functions[i].func_id, 8);
                module->addMux(NEW_ID, func_id_mux, func_id_const,
                    valid_1bit[i], mux_out);
                func_id_mux = RTLIL::SigSpec(mux_out);
            }
            module->connect(RTLIL::SigSpec(dpi_func_id), func_id_mux);

            RTLIL::SigSpec args_mux = RTLIL::SigSpec(RTLIL::State::S0, max_arg_width);
            for (int i = (int)functions.size() - 1; i >= 0; i--) {
                RTLIL::Wire *mux_out = module->addWire(NEW_ID, max_arg_width);
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

            for (const auto &func : functions) {
                if (GetSize(func.result_sig) > 0) {
                    module->connect(func.result_sig,
                        RTLIL::SigSpec(dpi_result_in).extract(0, GetSize(func.result_sig)));
                }

                module->remove(func.cell);

                log("    Converted to bridge: func_id=%d, arg_width=%d, ret_width=%d\n",
                    func.func_id, func.arg_width, func.ret_width);
                log("    Base address: 0x%04x\n", LOOM_DPI_BASE + func.func_id * FUNC_BLOCK_ALIGN);
            }
        }

        module->fixup_ports();
    }

    void generate_host_wrapper(const std::vector<DpiFunction> &functions) {
        log("\nLoom Instrument Interface Summary:\n");
        log("// Flop enable: emu_top controls loom_en to freeze DUT\n");
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

            if (func.ret_width > 0) {
                ofs << "      \"return\": {\n";
                ofs << "        \"type\": \"" << func.ret_type << "\",\n";
                ofs << "        \"width\": " << func.ret_width << "\n";
                ofs << "      },\n";
            } else {
                ofs << "      \"return\": null,\n";
            }

            ofs << "      \"args\": [\n";
            for (size_t j = 0; j < func.args.size(); j++) {
                const auto &arg = func.args[j];
                ofs << "        {\n";
                ofs << "          \"name\": \"" << arg.name << "\",\n";
                ofs << "          \"direction\": \"" << arg.direction << "\",\n";
                ofs << "          \"type\": \"" << arg.type << "\",\n";
                ofs << "          \"width\": " << arg.width;
                if (!arg.string_value.empty()) {
                    ofs << ",\n          \"value\": \"" << arg.string_value << "\"";
                }
                ofs << "\n";
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
        ofs << "// Generated by Loom loom_instrument pass - DO NOT EDIT\n";
        ofs << "//\n";
        ofs << "// DPI function dispatch table and wrappers.\n";
        ofs << "// Link this with your DPI implementation.\n";
        ofs << "\n";
        ofs << "#include <stdint.h>\n";
        ofs << "#include <loom_dpi_service.h>\n";
        ofs << "\n";

        ofs << "#include <stdio.h>\n\n";

        ofs << "// User-provided DPI function implementations\n";
        for (const auto &func : functions) {
            if (func.builtin) continue;  // builtins have no extern decl

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

        ofs << "// Wrapper functions for uniform callback interface\n";
        for (const auto &func : functions) {
            ofs << "static uint64_t _loom_wrap_" << func.name << "(const uint32_t *args) {\n";

            if (func.builtin) {
                // Built-in display function: generate printf inline
                // First arg (idx 0) is the format string (compile-time constant)
                std::string fmt_str;
                int arg_offset = 0;
                for (const auto &arg : func.args) {
                    if (arg.type == "string") {
                        fmt_str = arg.string_value;
                    }
                }

                ofs << "    printf(\"" << fmt_str << "\"";
                for (size_t i = 0; i < func.args.size(); i++) {
                    const auto &arg = func.args[i];
                    if (arg.type == "string") continue;  // skip format string
                    std::string c_type = sv_type_to_c(arg.type, arg.width);
                    ofs << ", (" << c_type << ")args[" << arg_offset << "]";
                    arg_offset += (arg.width + 31) / 32;
                }
                ofs << ");\n";
                ofs << "    return 0;\n";
            } else {
                // User function: generate call with args
                std::string call_args;
                int arg_offset = 0;
                for (size_t i = 0; i < func.args.size(); i++) {
                    const auto &arg = func.args[i];
                    if (i > 0) call_args += ", ";
                    if (arg.type == "string") {
                        call_args += "\"" + arg.string_value + "\"";
                    } else {
                        std::string c_type = sv_type_to_c(arg.type, arg.width);
                        call_args += "(" + c_type + ")args[" + std::to_string(arg_offset) + "]";
                        arg_offset += (arg.width + 31) / 32;
                    }
                }

                if (func.ret_width > 0) {
                    ofs << "    return (uint64_t)" << func.name << "(" << call_args << ");\n";
                } else {
                    ofs << "    " << func.name << "(" << call_args << ");\n";
                    ofs << "    return 0;\n";
                }
            }
            ofs << "}\n\n";
        }

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

LoomInstrumentPass LoomInstrumentPass_singleton;

PRIVATE_NAMESPACE_END
