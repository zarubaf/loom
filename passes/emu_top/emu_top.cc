// SPDX-License-Identifier: Apache-2.0
/*
 * emu_top - Yosys pass for emulation wrapper generation
 *
 * This pass generates a top-level wrapper module that:
 *   1. Instantiates the transformed DUT
 *   2. Gates all clocks when DPI call pending and host hasn't acked
 *   3. Exposes DPI and scan interfaces to external host
 *
 * Architecture:
 *   clk_external -> [loom_clk_gate] -> clk_gated -> DUT
 *                         ^
 *                         | CE = !dpi_valid | dpi_ack
 *
 * When a DPI call is pending (dpi_valid=1) and the host hasn't
 * acknowledged (dpi_ack=0), the clock is gated. The host asserts
 * dpi_ack when the result is ready, releasing the clock.
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct EmuTopPass : public Pass {
    EmuTopPass() : Pass("emu_top", "Generate emulation wrapper with clock gating") {}

    void help() override {
        log("\n");
        log("    emu_top [options] [selection]\n");
        log("\n");
        log("Generate a top-level emulation wrapper module.\n");
        log("\n");
        log("    -top <module>\n");
        log("        Specify the DUT module to wrap (required)\n");
        log("\n");
        log("    -wrapper <name>\n");
        log("        Name for the generated wrapper (default: emu_top_<dut>)\n");
        log("\n");
        log("    -clk <signal>\n");
        log("        Name of the clock signal in DUT (default: clk)\n");
        log("\n");
        log("    -rst <signal>\n");
        log("        Name of the reset signal in DUT (default: rst)\n");
        log("\n");
        log("The wrapper instantiates:\n");
        log("  - loom_clk_gate: gates clock when DPI call pending\n");
        log("  - loom_host_stub: simulation model for host (optional)\n");
        log("\n");
        log("Generated ports:\n");
        log("  - clk:            External clock input\n");
        log("  - rst:            External reset input\n");
        log("  - All DUT ports (except clock, which is gated)\n");
        log("  - loom_dpi_valid: DPI call pending (output)\n");
        log("  - loom_dpi_ack:   Host acknowledges result ready (input)\n");
        log("  - loom_dpi_*:     Other DPI interface signals\n");
        log("  - loom_scan_*:    Scan interface (directly exposed)\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing EMU_TOP pass.\n");

        std::string top_name;
        std::string wrapper_name;
        std::string clk_name = "clk";
        std::string rst_name = "rst";

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-top" && argidx + 1 < args.size()) {
                top_name = args[++argidx];
                continue;
            }
            if (args[argidx] == "-wrapper" && argidx + 1 < args.size()) {
                wrapper_name = args[++argidx];
                continue;
            }
            if (args[argidx] == "-clk" && argidx + 1 < args.size()) {
                clk_name = args[++argidx];
                continue;
            }
            if (args[argidx] == "-rst" && argidx + 1 < args.size()) {
                rst_name = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        if (top_name.empty()) {
            log_error("No top module specified. Use -top <module>\n");
        }

        // Find the DUT module
        RTLIL::Module *dut = design->module(RTLIL::escape_id(top_name));
        if (!dut) {
            log_error("Module '%s' not found\n", top_name.c_str());
        }

        if (wrapper_name.empty()) {
            wrapper_name = "emu_top_" + top_name;
        }

        log("Creating wrapper '%s' for DUT '%s'\n", wrapper_name.c_str(), top_name.c_str());
        log("  Clock signal: %s\n", clk_name.c_str());
        log("  Reset signal: %s\n", rst_name.c_str());

        // Create the wrapper module
        RTLIL::Module *wrapper = design->addModule(RTLIL::escape_id(wrapper_name));

        // Analyze DUT ports
        RTLIL::Wire *dut_clk = dut->wire(RTLIL::escape_id(clk_name));
        RTLIL::Wire *dut_rst = dut->wire(RTLIL::escape_id(rst_name));
        // Check both escaped and non-escaped names for DPI valid
        RTLIL::Wire *dut_dpi_valid = dut->wire(ID(loom_dpi_valid));
        if (!dut_dpi_valid) dut_dpi_valid = dut->wire(ID(\\loom_dpi_valid));

        if (!dut_clk) {
            log_warning("Clock port '%s' not found in DUT\n", clk_name.c_str());
        }

        // Create wrapper ports
        RTLIL::Wire *ext_clk = wrapper->addWire(RTLIL::escape_id(clk_name), 1);
        ext_clk->port_input = true;

        RTLIL::Wire *ext_rst = nullptr;
        if (dut_rst) {
            ext_rst = wrapper->addWire(RTLIL::escape_id(rst_name), 1);
            ext_rst->port_input = true;
        }

        // Create gated clock wire
        RTLIL::Wire *clk_gated = wrapper->addWire(ID(clk_gated), 1);

        // Create clock enable wire (!dpi_valid)
        RTLIL::Wire *clk_enable = wrapper->addWire(ID(clk_enable), 1);

        // Create DPI interface wires in wrapper (directly exposed)
        RTLIL::Wire *w_dpi_valid = nullptr;
        RTLIL::Wire *w_dpi_ack = nullptr;
        RTLIL::Wire *w_dpi_func_id = nullptr;
        RTLIL::Wire *w_dpi_args = nullptr;
        RTLIL::Wire *w_dpi_result = nullptr;

        if (dut_dpi_valid) {
            // DPI interface present - create wrapper ports
            // Check both escaped and non-escaped names for DUT ports
            RTLIL::Wire *dut_func_id = dut->wire(ID(loom_dpi_func_id));
            if (!dut_func_id) dut_func_id = dut->wire(ID(\\loom_dpi_func_id));
            RTLIL::Wire *dut_args = dut->wire(ID(loom_dpi_args));
            if (!dut_args) dut_args = dut->wire(ID(\\loom_dpi_args));
            RTLIL::Wire *dut_result = dut->wire(ID(loom_dpi_result));
            if (!dut_result) dut_result = dut->wire(ID(\\loom_dpi_result));

            w_dpi_valid = wrapper->addWire(ID(loom_dpi_valid), 1);
            w_dpi_valid->port_output = true;

            // dpi_ack is an input to the wrapper (from host) - NOT connected to DUT
            // It's only used for clock gating control in emu_top
            w_dpi_ack = wrapper->addWire(ID(loom_dpi_ack), 1);
            w_dpi_ack->port_input = true;

            if (dut_func_id) {
                w_dpi_func_id = wrapper->addWire(ID(loom_dpi_func_id), GetSize(dut_func_id));
                w_dpi_func_id->port_output = true;
            }

            if (dut_args) {
                w_dpi_args = wrapper->addWire(ID(loom_dpi_args), GetSize(dut_args));
                w_dpi_args->port_output = true;
            }

            if (dut_result) {
                w_dpi_result = wrapper->addWire(ID(loom_dpi_result), GetSize(dut_result));
                w_dpi_result->port_input = true;
            }

            log("  DPI interface detected:\n");
            log("    loom_dpi_valid:   output (1 bit)\n");
            log("    loom_dpi_ack:     input (1 bit)\n");
            if (dut_func_id) log("    loom_dpi_func_id: output (%d bits)\n", GetSize(dut_func_id));
            if (dut_args) log("    loom_dpi_args:    output (%d bits)\n", GetSize(dut_args));
            if (dut_result) log("    loom_dpi_result:  input (%d bits)\n", GetSize(dut_result));
        } else {
            log("  No DPI interface detected in DUT\n");
        }

        // Create scan interface wires in wrapper (if present in DUT)
        // Check both escaped and non-escaped names
        RTLIL::Wire *dut_scan_enable = dut->wire(ID(loom_scan_enable));
        if (!dut_scan_enable) dut_scan_enable = dut->wire(ID(\\loom_scan_enable));
        RTLIL::Wire *dut_scan_in = dut->wire(ID(loom_scan_in));
        if (!dut_scan_in) dut_scan_in = dut->wire(ID(\\loom_scan_in));
        RTLIL::Wire *dut_scan_out = dut->wire(ID(loom_scan_out));
        if (!dut_scan_out) dut_scan_out = dut->wire(ID(\\loom_scan_out));

        RTLIL::Wire *w_scan_enable = nullptr;
        RTLIL::Wire *w_scan_in = nullptr;
        RTLIL::Wire *w_scan_out = nullptr;

        if (dut_scan_enable || dut_scan_in || dut_scan_out) {
            log("  Scan interface detected:\n");

            if (dut_scan_enable) {
                w_scan_enable = wrapper->addWire(ID(loom_scan_enable), 1);
                w_scan_enable->port_input = true;
                log("    loom_scan_enable: input (1 bit)\n");
            }

            if (dut_scan_in) {
                w_scan_in = wrapper->addWire(ID(loom_scan_in), 1);
                w_scan_in->port_input = true;
                log("    loom_scan_in:     input (1 bit)\n");
            }

            if (dut_scan_out) {
                w_scan_out = wrapper->addWire(ID(loom_scan_out), 1);
                w_scan_out->port_output = true;
                log("    loom_scan_out:    output (1 bit)\n");
            }
        }

        // Copy other DUT ports to wrapper (except clk, rst, loom_* which are handled specially)
        for (auto wire : dut->wires()) {
            if (!wire->port_input && !wire->port_output)
                continue;

            // Skip clock and reset (handled above)
            if (wire->name == RTLIL::escape_id(clk_name) ||
                wire->name == RTLIL::escape_id(rst_name)) {
                continue;
            }

            // Skip loom_* signals (handled above)
            // Check for "loom_" anywhere in name (covers escaped \loom_ and non-escaped loom_)
            std::string wire_name = wire->name.str();
            if (wire_name.find("loom_") != std::string::npos) {
                continue;
            }

            // Skip if already exists in wrapper (safety check)
            if (wrapper->wire(wire->name)) {
                continue;
            }

            RTLIL::Wire *w = wrapper->addWire(wire->name, GetSize(wire));
            w->port_input = wire->port_input;
            w->port_output = wire->port_output;
        }

        wrapper->fixup_ports();

        // === Instantiate loom_clk_gate ===
        // Module: loom_clk_gate(clk_i, ce_i, clk_o) - lowRISC style
        // CE = !dpi_valid | dpi_ack (clock runs when no call OR host has acked)
        RTLIL::Cell *clk_gate = wrapper->addCell(ID(u_clk_gate), ID(loom_clk_gate));
        clk_gate->setPort(ID(clk_i), RTLIL::SigSpec(ext_clk));
        clk_gate->setPort(ID(ce_i), RTLIL::SigSpec(clk_enable));
        clk_gate->setPort(ID(clk_o), RTLIL::SigSpec(clk_gated));

        // Generate clock enable: CE = !dpi_valid | dpi_ack
        // This means: clock enabled when no DPI call pending OR when host has acknowledged
        if (w_dpi_valid && w_dpi_ack) {
            // Create intermediate wire for !dpi_valid
            RTLIL::Wire *not_dpi_valid = wrapper->addWire(NEW_ID, 1);
            // dpi_valid_tap will be created below when connecting DUT
            RTLIL::Wire *dpi_valid_tap = wrapper->addWire(ID(dpi_valid_tap), 1);

            // !dpi_valid
            wrapper->addNot(NEW_ID, RTLIL::SigSpec(dpi_valid_tap), RTLIL::SigSpec(not_dpi_valid));

            // clk_enable = !dpi_valid | dpi_ack
            wrapper->addOr(NEW_ID, RTLIL::SigSpec(not_dpi_valid), RTLIL::SigSpec(w_dpi_ack),
                          RTLIL::SigSpec(clk_enable));
        } else {
            // No DPI - clock always enabled
            wrapper->connect(RTLIL::SigSpec(clk_enable), RTLIL::SigSpec(RTLIL::State::S1));
        }

        // === Instantiate DUT ===
        RTLIL::Cell *dut_inst = wrapper->addCell(ID(u_dut), dut->name);

        // Connect DUT ports
        for (auto wire : dut->wires()) {
            if (!wire->port_input && !wire->port_output)
                continue;

            std::string wire_name = wire->name.str();

            // Handle clock specially - connect to gated clock
            if (wire->name == RTLIL::escape_id(clk_name)) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(clk_gated));
                continue;
            }

            // Handle reset
            if (wire->name == RTLIL::escape_id(rst_name) && ext_rst) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(ext_rst));
                continue;
            }

            // Handle DPI valid - skip here, handled separately below for clock gating
            if ((wire->name == ID(loom_dpi_valid) || wire->name == ID(\\loom_dpi_valid)) && w_dpi_valid) {
                continue;  // Handled in dpi_valid_tap section below
            }

            // Note: loom_dpi_ack is NOT connected to DUT - it's only for emu_top clock gating
            // The DUT is clock-gated and transparent to the handshaking

            if ((wire->name == ID(loom_dpi_func_id) || wire->name == ID(\\loom_dpi_func_id)) && w_dpi_func_id) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w_dpi_func_id));
                continue;
            }

            if ((wire->name == ID(loom_dpi_args) || wire->name == ID(\\loom_dpi_args)) && w_dpi_args) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w_dpi_args));
                continue;
            }

            if ((wire->name == ID(loom_dpi_result) || wire->name == ID(\\loom_dpi_result)) && w_dpi_result) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w_dpi_result));
                continue;
            }

            // Handle scan signals
            if ((wire->name == ID(loom_scan_enable) || wire->name == ID(\\loom_scan_enable)) && w_scan_enable) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w_scan_enable));
                continue;
            }

            if ((wire->name == ID(loom_scan_in) || wire->name == ID(\\loom_scan_in)) && w_scan_in) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w_scan_in));
                continue;
            }

            if ((wire->name == ID(loom_scan_out) || wire->name == ID(\\loom_scan_out)) && w_scan_out) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w_scan_out));
                continue;
            }

            // Other ports - connect to wrapper ports
            RTLIL::Wire *w = wrapper->wire(wire->name);
            if (w) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(w));
            }
        }

        // Connect dpi_valid from DUT to wrapper output and clock gating logic
        // The dpi_valid_tap wire was created above in the clock gating section
        if (w_dpi_valid) {
            RTLIL::Wire *dpi_valid_tap = wrapper->wire(ID(dpi_valid_tap));
            if (dpi_valid_tap) {
                // Connect DUT's dpi_valid output to tap wire
                // Check both escaped and non-escaped names
                if (dut->wire(ID(loom_dpi_valid))) {
                    dut_inst->setPort(ID(loom_dpi_valid), RTLIL::SigSpec(dpi_valid_tap));
                } else {
                    dut_inst->setPort(ID(\\loom_dpi_valid), RTLIL::SigSpec(dpi_valid_tap));
                }

                // Connect tap to wrapper output
                wrapper->connect(RTLIL::SigSpec(w_dpi_valid), RTLIL::SigSpec(dpi_valid_tap));
            }
        }

        wrapper->fixup_ports();

        log("Generated wrapper module '%s'\n", wrapper_name.c_str());
        log("  Instantiated: loom_clk_gate (u_clk_gate)\n");
        log("  Instantiated: %s (u_dut)\n", top_name.c_str());
        log("  Clock gating: %s\n", w_dpi_valid ? "enabled (CE = !dpi_valid | dpi_ack)" : "disabled (no DPI)");
    }
};

EmuTopPass EmuTopPass_singleton;

PRIVATE_NAMESPACE_END
