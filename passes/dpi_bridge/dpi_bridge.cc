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
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// DPI function descriptor extracted from cells
struct DpiFunction {
    std::string name;
    int func_id;
    int arg_width;
    int ret_width;
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
        log("    -gen_wrapper\n");
        log("        Generate info about host software interface\n");
        log("\n");
        log("    -base_addr N\n");
        log("        Base address for mailbox registers (default: 0x1000)\n");
        log("\n");
        log("The $__loom_dpi_call cells have:\n");
        log("  - Attribute 'loom_dpi_func': DPI function name\n");
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
        int base_addr = 0x1000;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-gen_wrapper") {
                gen_wrapper = true;
                continue;
            }
            if (args[argidx] == "-base_addr" && argidx + 1 < args.size()) {
                base_addr = atoi(args[++argidx].c_str());
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

                // Get the argument and result signals
                if (cell->hasPort(ID(ARGS))) {
                    func.args_sig = cell->getPort(ID(ARGS));
                }
                if (cell->hasPort(ID(RESULT))) {
                    func.result_sig = cell->getPort(ID(RESULT));
                }

                dpi_functions.push_back(func);

                // Convert DPI call to bridge interface
                convert_to_bridge(module, func, base_addr);
            }
        }

        if (gen_wrapper && !dpi_functions.empty()) {
            generate_host_wrapper(dpi_functions, base_addr);
        }

        log("Processed %zu DPI function(s)\n", dpi_functions.size());
    }

    // Derive the execution condition for a DPI call by tracing how its result is used.
    // The DPI result typically flows through mux chains where the select signals
    // represent the conditions under which the result is actually needed.
    //
    // For example, if the DPI call is in: if (start && state==IDLE) sum <= dpi_func(a,b);
    // After proc, this becomes muxes:
    //   _18_ = start ? dpi_result : sum       (select = start)
    //   _20_ = !state ? _18_ : xxx            (select = !state, i.e., state==IDLE)
    // The select signals (start, !state) are ANDed to form the valid condition.
    RTLIL::SigSpec derive_valid_condition(RTLIL::Module *module, const DpiFunction &func) {
        SigMap sigmap(module);

        // Get the DPI result signal
        RTLIL::SigSpec result_sig = sigmap(func.result_sig);
        if (GetSize(result_sig) == 0) {
            log("    No result signal, defaulting to valid=1\n");
            return RTLIL::SigSpec(RTLIL::State::S1);
        }

        // Collect select conditions by tracing through the mux chain
        std::vector<RTLIL::SigSpec> select_conditions;
        pool<RTLIL::SigBit> signals_to_trace;
        pool<RTLIL::SigBit> traced_signals;

        // Start with the DPI result signal bits
        for (auto bit : result_sig.bits()) {
            signals_to_trace.insert(bit);
        }

        // Trace through mux chain - find all muxes that gate the DPI result
        // and collect their select signals
        while (!signals_to_trace.empty()) {
            pool<RTLIL::SigBit> next_signals;

            for (auto cell : module->cells()) {
                if (cell->type != ID($mux))
                    continue;

                RTLIL::SigSpec port_b = sigmap(cell->getPort(ID::B));
                RTLIL::SigSpec port_y = sigmap(cell->getPort(ID::Y));

                // Check if any signal we're tracing is used in port B (selected when S=1)
                bool uses_traced_signal = false;
                for (auto bit : port_b.bits()) {
                    if (signals_to_trace.count(bit)) {
                        uses_traced_signal = true;
                        break;
                    }
                }

                if (uses_traced_signal) {
                    RTLIL::SigSpec sel = cell->getPort(ID::S);

                    // Only add if we haven't seen this select before
                    bool already_have = false;
                    RTLIL::SigSpec mapped_sel = sigmap(sel);
                    for (auto &existing : select_conditions) {
                        if (sigmap(existing) == mapped_sel) {
                            already_have = true;
                            break;
                        }
                    }

                    if (!already_have) {
                        log("    Found mux in chain: %s, select=%s\n",
                            log_id(cell), log_signal(sel));
                        select_conditions.push_back(sel);
                    }

                    // Add mux output to signals to trace (follow the chain)
                    for (auto bit : port_y.bits()) {
                        if (!traced_signals.count(bit)) {
                            next_signals.insert(bit);
                        }
                    }
                }
            }

            // Mark current signals as traced
            for (auto bit : signals_to_trace) {
                traced_signals.insert(bit);
            }

            signals_to_trace = next_signals;
        }

        if (select_conditions.empty()) {
            log("    No mux conditions found, defaulting to valid=1\n");
            return RTLIL::SigSpec(RTLIL::State::S1);
        }

        // Combine all select conditions with AND
        // valid = sel1 & sel2 & sel3 & ...
        RTLIL::SigSpec valid = select_conditions[0];

        for (size_t i = 1; i < select_conditions.size(); i++) {
            RTLIL::Wire *and_out = module->addWire(NEW_ID, 1);
            module->addAnd(NEW_ID, valid, select_conditions[i], and_out);
            valid = RTLIL::SigSpec(and_out);
        }

        log("    Derived valid condition from %zu mux select(s)\n", select_conditions.size());
        return valid;
    }

    void convert_to_bridge(RTLIL::Module *module, const DpiFunction &func, int base_addr) {
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

        log("    Converted to bridge: func_id=%d, arg_width=%d, ret_width=%d\n",
            func.func_id, func.arg_width, func.ret_width);
        log("    Base address: 0x%x\n", base_addr + func.func_id * 0x100);
    }

    void generate_host_wrapper(const std::vector<DpiFunction> &functions, int base_addr) {
        log("\nDPI Bridge Interface Summary:\n");
        log("// Clock gating: emu_top gates all clocks when loom_dpi_valid=1\n");
        log("// Base address: 0x%x\n", base_addr);
        log("\n");

        for (const auto &func : functions) {
            int addr = base_addr + func.func_id * 0x100;
            log("// Function: %s (ID: %d)\n", func.name.c_str(), func.func_id);
            log("//   Args register:   0x%x (%d bits)\n", addr, func.arg_width);
            log("//   Result register: 0x%x (%d bits)\n", addr + 0x10, func.ret_width);
            log("\n");
        }
    }
};

DpiBridgePass DpiBridgePass_singleton;

PRIVATE_NAMESPACE_END
