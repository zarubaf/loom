/*
 * dpi_bridge - Yosys pass for DPI-C function bridge generation
 *
 * This pass converts DPI function call cells ($__loom_dpi_call) into hardware
 * mailbox interfaces that enable FPGA<->host communication for emulation.
 *
 * DPI functions cannot be synthesized directly. The yosys-slang frontend
 * automatically creates $__loom_dpi_call cells for DPI import calls, which
 * this pass then converts to hardware bridges that:
 *   1. Capture function arguments
 *   2. Signal the host via mailbox/interrupt
 *   3. Wait for host to compute result
 *   4. Return result to the design
 *
 * Bridge interface:
 *   - loom_dpi_req:     Request valid (FPGA->host)
 *   - loom_dpi_func_id: Function identifier
 *   - loom_dpi_args:    Packed function arguments
 *   - loom_dpi_ack:     Host acknowledgment (host->FPGA)
 *   - loom_dpi_result:  Return value from host
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
};

struct DpiBridgePass : public Pass {
    DpiBridgePass() : Pass("dpi_bridge", "Convert DPI placeholders to hardware bridges") {}

    void help() override {
        log("\n");
        log("    dpi_bridge [options] [selection]\n");
        log("\n");
        log("Convert DPI function call cells to hardware mailbox bridges.\n");
        log("\n");
        log("This pass processes $__loom_dpi_call cells created by yosys-slang\n");
        log("when it encounters DPI import function calls in SystemVerilog.\n");
        log("\n");
        log("    -gen_wrapper\n");
        log("        Generate Verilog wrapper module for host software interface\n");
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

    void convert_to_bridge(RTLIL::Module *module, const DpiFunction &func, int base_addr) {
        RTLIL::Cell *cell = func.cell;

        // Create bridge interface ports if they don't exist
        // Use loom_ prefix to avoid name conflicts with user signals
        RTLIL::Wire *dpi_req = module->wire(ID(\\loom_dpi_req));
        if (!dpi_req) {
            dpi_req = module->addWire(ID(\\loom_dpi_req), 1);
            dpi_req->port_output = true;
        }

        RTLIL::Wire *dpi_ack = module->wire(ID(\\loom_dpi_ack));
        if (!dpi_ack) {
            dpi_ack = module->addWire(ID(\\loom_dpi_ack), 1);
            dpi_ack->port_input = true;
        }

        RTLIL::Wire *dpi_func_id = module->wire(ID(\\loom_dpi_func_id));
        if (!dpi_func_id) {
            dpi_func_id = module->addWire(ID(\\loom_dpi_func_id), 8);
            dpi_func_id->port_output = true;
        }

        // Create or extend args output port
        RTLIL::Wire *dpi_args_out = module->wire(ID(\\loom_dpi_args));
        if (!dpi_args_out) {
            dpi_args_out = module->addWire(ID(\\loom_dpi_args), func.arg_width);
            dpi_args_out->port_output = true;
        } else if (GetSize(dpi_args_out) < func.arg_width) {
            // Extend width if needed for this function
            dpi_args_out->width = func.arg_width;
        }

        // Create or extend result input port
        RTLIL::Wire *dpi_result_in = module->wire(ID(\\loom_dpi_result));
        if (!dpi_result_in) {
            dpi_result_in = module->addWire(ID(\\loom_dpi_result), func.ret_width);
            dpi_result_in->port_input = true;
        } else if (GetSize(dpi_result_in) < func.ret_width) {
            dpi_result_in->width = func.ret_width;
        }

        // Create a request signal for this specific DPI call
        // For now, we tie dpi_req high when the cell exists (combinational)
        // In a full implementation, this would be controlled by state machine
        RTLIL::Wire *req_wire = module->addWire(NEW_ID, 1);
        module->connect(RTLIL::SigSpec(req_wire), RTLIL::SigSpec(RTLIL::State::S1));
        module->connect(RTLIL::SigSpec(dpi_req), RTLIL::SigSpec(req_wire));

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
        log("\nGenerating host software interface:\n");
        log("// DPI Bridge Host Interface\n");
        log("// Base address: 0x%x\n", base_addr);
        log("\n");

        for (const auto &func : functions) {
            int addr = base_addr + func.func_id * 0x100;
            log("// Function: %s (ID: %d)\n", func.name.c_str(), func.func_id);
            log("//   Args register:   0x%x (%d bits)\n", addr, func.arg_width);
            log("//   Result register: 0x%x (%d bits)\n", addr + 0x10, func.ret_width);
            log("//   Control/Status:  0x%x\n", addr + 0x20);
            log("\n");
        }
    }
};

DpiBridgePass DpiBridgePass_singleton;

PRIVATE_NAMESPACE_END
