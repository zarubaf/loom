// SPDX-License-Identifier: Apache-2.0
/*
 * emu_top - Yosys pass for complete emulation wrapper generation
 *
 * This pass generates a complete emulation top-level module `loom_emu_top` that includes:
 *   1. The transformed DUT (after loom_instrument pass)
 *   2. loom_emu_ctrl: emulation controller + DPI bridge
 *   3. loom_axi_interconnect: routes AXI-Lite to ctrl and regfile
 *   4. loom_dpi_regfile: DPI call registers
 *   5. loom_scan_ctrl: scan chain controller
 *
 * The DUT clock runs free (ungated). State freezing is done via loom_en_o
 * from emu_ctrl which owns the enable signal end-to-end.
 *
 * The generated module exposes only:
 *   - clk_i, rst_ni: clock and reset
 *   - AXI-Lite slave interface: for host communication
 *   - IRQ output
 *
 * All DUT inputs (except clk/rst/loom_*) are tied to '0.
 * All DUT outputs (except loom_*) are left open.
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct EmuTopPass : public Pass {
    EmuTopPass() : Pass("emu_top", "Generate complete emulation wrapper with all infrastructure") {}

    void help() override {
        log("\n");
        log("    emu_top [options] [selection]\n");
        log("\n");
        log("Generate a complete emulation top-level wrapper module.\n");
        log("\n");
        log("    -top <module>\n");
        log("        Specify the DUT module to wrap (required)\n");
        log("\n");
        log("    -clk <signal>\n");
        log("        Name of the clock signal in DUT (default: clk_i)\n");
        log("\n");
        log("    -rst <signal>\n");
        log("        Name of the reset signal in DUT (default: rst_ni)\n");
        log("\n");
        log("    -addr_width <bits>\n");
        log("        AXI-Lite address width (default: 20)\n");
        log("\n");
        log("    -n_irq <count>\n");
        log("        Number of IRQ lines (default: 16)\n");
        log("\n");
        log("DPI function count and scan chain length are auto-detected from\n");
        log("module attributes set by loom_instrument and scan_insert.\n");
        log("\n");
        log("Generated module: loom_emu_top\n");
        log("Exposed ports:\n");
        log("  - clk_i:      Clock input\n");
        log("  - rst_ni:     Active-low reset\n");
        log("  - s_axil_*:   AXI-Lite slave interface\n");
        log("  - irq_o:      Interrupt output\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override {
        log_header(design, "Executing EMU_TOP pass (complete wrapper).\n");

        std::string top_name;
        std::string clk_name = "clk_i";
        std::string rst_name = "rst_ni";
        int addr_width = 20;
        int n_irq = 16;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-top" && argidx + 1 < args.size()) {
                top_name = args[++argidx];
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
            if (args[argidx] == "-addr_width" && argidx + 1 < args.size()) {
                addr_width = atoi(args[++argidx].c_str());
                continue;
            }
            if (args[argidx] == "-n_irq" && argidx + 1 < args.size()) {
                n_irq = atoi(args[++argidx].c_str());
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

        // Auto-detect DPI function count from loom_instrument attribute
        int n_dpi_funcs = 0;
        std::string n_dpi_str = dut->get_string_attribute(ID(loom_n_dpi_funcs));
        if (!n_dpi_str.empty()) {
            n_dpi_funcs = atoi(n_dpi_str.c_str());
        }

        // Auto-detect scan chain length from scan_insert attribute
        int scan_chain_length = 0;
        std::string scan_len_str = dut->get_string_attribute(ID(loom_scan_chain_length));
        if (!scan_len_str.empty()) {
            scan_chain_length = atoi(scan_len_str.c_str());
        }

        log("Creating loom_emu_top wrapper for DUT '%s'\n", top_name.c_str());
        log("  Clock: %s, Reset: %s\n", clk_name.c_str(), rst_name.c_str());
        log("  DPI functions: %d (auto-detected)\n", n_dpi_funcs);
        log("  Scan chain: %d bits (auto-detected)\n", scan_chain_length);

        // Create the wrapper module
        RTLIL::Module *wrapper = design->addModule(ID(loom_emu_top));

        // =========================================================================
        // Create wrapper ports
        // =========================================================================

        // Clock and reset
        RTLIL::Wire *clk_i = wrapper->addWire(ID(clk_i), 1);
        clk_i->port_input = true;

        RTLIL::Wire *rst_ni = wrapper->addWire(ID(rst_ni), 1);
        rst_ni->port_input = true;

        // AXI-Lite slave interface (from BFM)
        RTLIL::Wire *s_axil_araddr = wrapper->addWire(ID(s_axil_araddr_i), addr_width);
        s_axil_araddr->port_input = true;
        RTLIL::Wire *s_axil_arvalid = wrapper->addWire(ID(s_axil_arvalid_i), 1);
        s_axil_arvalid->port_input = true;
        RTLIL::Wire *s_axil_arready = wrapper->addWire(ID(s_axil_arready_o), 1);
        s_axil_arready->port_output = true;
        RTLIL::Wire *s_axil_rdata = wrapper->addWire(ID(s_axil_rdata_o), 32);
        s_axil_rdata->port_output = true;
        RTLIL::Wire *s_axil_rresp = wrapper->addWire(ID(s_axil_rresp_o), 2);
        s_axil_rresp->port_output = true;
        RTLIL::Wire *s_axil_rvalid = wrapper->addWire(ID(s_axil_rvalid_o), 1);
        s_axil_rvalid->port_output = true;
        RTLIL::Wire *s_axil_rready = wrapper->addWire(ID(s_axil_rready_i), 1);
        s_axil_rready->port_input = true;

        RTLIL::Wire *s_axil_awaddr = wrapper->addWire(ID(s_axil_awaddr_i), addr_width);
        s_axil_awaddr->port_input = true;
        RTLIL::Wire *s_axil_awvalid = wrapper->addWire(ID(s_axil_awvalid_i), 1);
        s_axil_awvalid->port_input = true;
        RTLIL::Wire *s_axil_awready = wrapper->addWire(ID(s_axil_awready_o), 1);
        s_axil_awready->port_output = true;
        RTLIL::Wire *s_axil_wdata = wrapper->addWire(ID(s_axil_wdata_i), 32);
        s_axil_wdata->port_input = true;
        RTLIL::Wire *s_axil_wstrb = wrapper->addWire(ID(s_axil_wstrb_i), 4);
        s_axil_wstrb->port_input = true;
        RTLIL::Wire *s_axil_wvalid = wrapper->addWire(ID(s_axil_wvalid_i), 1);
        s_axil_wvalid->port_input = true;
        RTLIL::Wire *s_axil_wready = wrapper->addWire(ID(s_axil_wready_o), 1);
        s_axil_wready->port_output = true;
        RTLIL::Wire *s_axil_bresp = wrapper->addWire(ID(s_axil_bresp_o), 2);
        s_axil_bresp->port_output = true;
        RTLIL::Wire *s_axil_bvalid = wrapper->addWire(ID(s_axil_bvalid_o), 1);
        s_axil_bvalid->port_output = true;
        RTLIL::Wire *s_axil_bready = wrapper->addWire(ID(s_axil_bready_i), 1);
        s_axil_bready->port_input = true;

        // IRQ output
        RTLIL::Wire *irq_o = wrapper->addWire(ID(irq_o), n_irq);
        irq_o->port_output = true;

        // Finish output (triggers simulation shutdown)
        RTLIL::Wire *finish_o = wrapper->addWire(ID(finish_o), 1);
        finish_o->port_output = true;

        wrapper->fixup_ports();

        // =========================================================================
        // Create internal wires
        // =========================================================================

        // Interconnect -> emu_ctrl (slave 0)
        RTLIL::Wire *m0_araddr = wrapper->addWire(ID(m0_araddr), 8);
        RTLIL::Wire *m0_arvalid = wrapper->addWire(ID(m0_arvalid), 1);
        RTLIL::Wire *m0_arready = wrapper->addWire(ID(m0_arready), 1);
        RTLIL::Wire *m0_rdata = wrapper->addWire(ID(m0_rdata), 32);
        RTLIL::Wire *m0_rresp = wrapper->addWire(ID(m0_rresp), 2);
        RTLIL::Wire *m0_rvalid = wrapper->addWire(ID(m0_rvalid), 1);
        RTLIL::Wire *m0_rready = wrapper->addWire(ID(m0_rready), 1);
        RTLIL::Wire *m0_awaddr = wrapper->addWire(ID(m0_awaddr), 8);
        RTLIL::Wire *m0_awvalid = wrapper->addWire(ID(m0_awvalid), 1);
        RTLIL::Wire *m0_awready = wrapper->addWire(ID(m0_awready), 1);
        RTLIL::Wire *m0_wdata = wrapper->addWire(ID(m0_wdata), 32);
        RTLIL::Wire *m0_wvalid = wrapper->addWire(ID(m0_wvalid), 1);
        RTLIL::Wire *m0_wready = wrapper->addWire(ID(m0_wready), 1);
        RTLIL::Wire *m0_bresp = wrapper->addWire(ID(m0_bresp), 2);
        RTLIL::Wire *m0_bvalid = wrapper->addWire(ID(m0_bvalid), 1);
        RTLIL::Wire *m0_bready = wrapper->addWire(ID(m0_bready), 1);

        // Interconnect -> dpi_regfile (slave 1)
        RTLIL::Wire *m1_araddr = wrapper->addWire(ID(m1_araddr), 16);
        RTLIL::Wire *m1_arvalid = wrapper->addWire(ID(m1_arvalid), 1);
        RTLIL::Wire *m1_arready = wrapper->addWire(ID(m1_arready), 1);
        RTLIL::Wire *m1_rdata = wrapper->addWire(ID(m1_rdata), 32);
        RTLIL::Wire *m1_rresp = wrapper->addWire(ID(m1_rresp), 2);
        RTLIL::Wire *m1_rvalid = wrapper->addWire(ID(m1_rvalid), 1);
        RTLIL::Wire *m1_rready = wrapper->addWire(ID(m1_rready), 1);
        RTLIL::Wire *m1_awaddr = wrapper->addWire(ID(m1_awaddr), 16);
        RTLIL::Wire *m1_awvalid = wrapper->addWire(ID(m1_awvalid), 1);
        RTLIL::Wire *m1_awready = wrapper->addWire(ID(m1_awready), 1);
        RTLIL::Wire *m1_wdata = wrapper->addWire(ID(m1_wdata), 32);
        RTLIL::Wire *m1_wvalid = wrapper->addWire(ID(m1_wvalid), 1);
        RTLIL::Wire *m1_wready = wrapper->addWire(ID(m1_wready), 1);
        RTLIL::Wire *m1_bresp = wrapper->addWire(ID(m1_bresp), 2);
        RTLIL::Wire *m1_bvalid = wrapper->addWire(ID(m1_bvalid), 1);
        RTLIL::Wire *m1_bready = wrapper->addWire(ID(m1_bready), 1);

        // Interconnect -> scan_ctrl (slave 2)
        RTLIL::Wire *m2_araddr = wrapper->addWire(ID(m2_araddr), 12);
        RTLIL::Wire *m2_arvalid = wrapper->addWire(ID(m2_arvalid), 1);
        RTLIL::Wire *m2_arready = wrapper->addWire(ID(m2_arready), 1);
        RTLIL::Wire *m2_rdata = wrapper->addWire(ID(m2_rdata), 32);
        RTLIL::Wire *m2_rresp = wrapper->addWire(ID(m2_rresp), 2);
        RTLIL::Wire *m2_rvalid = wrapper->addWire(ID(m2_rvalid), 1);
        RTLIL::Wire *m2_rready = wrapper->addWire(ID(m2_rready), 1);
        RTLIL::Wire *m2_awaddr = wrapper->addWire(ID(m2_awaddr), 12);
        RTLIL::Wire *m2_awvalid = wrapper->addWire(ID(m2_awvalid), 1);
        RTLIL::Wire *m2_awready = wrapper->addWire(ID(m2_awready), 1);
        RTLIL::Wire *m2_wdata = wrapper->addWire(ID(m2_wdata), 32);
        RTLIL::Wire *m2_wvalid = wrapper->addWire(ID(m2_wvalid), 1);
        RTLIL::Wire *m2_wready = wrapper->addWire(ID(m2_wready), 1);
        RTLIL::Wire *m2_bresp = wrapper->addWire(ID(m2_bresp), 2);
        RTLIL::Wire *m2_bvalid = wrapper->addWire(ID(m2_bvalid), 1);
        RTLIL::Wire *m2_bready = wrapper->addWire(ID(m2_bready), 1);

        // Scan chain signals
        RTLIL::Wire *scan_enable = wrapper->addWire(ID(scan_enable), 1);
        RTLIL::Wire *scan_in = wrapper->addWire(ID(scan_in), 1);
        RTLIL::Wire *scan_out = wrapper->addWire(ID(scan_out), 1);
        RTLIL::Wire *scan_busy = wrapper->addWire(ID(scan_busy), 1);

        // emu_ctrl signals
        RTLIL::Wire *loom_en_wire = wrapper->addWire(ID(loom_en_wire), 1);
        RTLIL::Wire *dut_rst_n = wrapper->addWire(ID(dut_rst_n), 1);
        RTLIL::Wire *cycle_count = wrapper->addWire(ID(cycle_count), 64);
        RTLIL::Wire *irq_state_change = wrapper->addWire(ID(irq_state_change), 1);
        RTLIL::Wire *emu_finish = wrapper->addWire(ID(emu_finish), 1);
        RTLIL::Wire *dut_finish = wrapper->addWire(ID(dut_finish), 1);

        // DPI regfile <-> emu_ctrl signals
        int max_args = 8;
        int n_dpi = n_dpi_funcs > 0 ? n_dpi_funcs : 1;
        RTLIL::Wire *dpi_call_valid = wrapper->addWire(ID(dpi_call_valid), n_dpi);
        RTLIL::Wire *dpi_call_ready = wrapper->addWire(ID(dpi_call_ready), n_dpi);
        int args_width = n_dpi * max_args * 32;
        RTLIL::Wire *dpi_call_args = wrapper->addWire(ID(dpi_call_args), args_width);
        RTLIL::Wire *dpi_ret_valid = wrapper->addWire(ID(dpi_ret_valid), n_dpi);
        RTLIL::Wire *dpi_ret_ready = wrapper->addWire(ID(dpi_ret_ready), n_dpi);
        int ret_data_per_func = 64 + max_args * 32; // scalar result + host-written args
        RTLIL::Wire *dpi_ret_data = wrapper->addWire(ID(dpi_ret_data), n_dpi * ret_data_per_func);
        RTLIL::Wire *dpi_stall = wrapper->addWire(ID(dpi_stall), n_dpi);

        // DUT DPI interface signals (connected through emu_ctrl)
        // Read actual widths from DUT's loom_dpi_args/result ports
        int dut_args_width = 64;   // default
        int dut_result_width = 32; // default
        for (auto wire : dut->wires()) {
            std::string wn = wire->name.str();
            if (wn.find("loom_dpi_args") != std::string::npos && wire->port_output)
                dut_args_width = wire->width;
            if (wn.find("loom_dpi_result") != std::string::npos && wire->port_input)
                dut_result_width = wire->width;
        }
        RTLIL::Wire *dut_dpi_valid_w = wrapper->addWire(ID(dut_dpi_valid), 1);
        RTLIL::Wire *dut_dpi_ack = wrapper->addWire(ID(dut_dpi_ack), 1);
        RTLIL::Wire *dut_dpi_func_id = wrapper->addWire(ID(dut_dpi_func_id), 8);
        RTLIL::Wire *dut_dpi_args = wrapper->addWire(ID(dut_dpi_args), dut_args_width);
        RTLIL::Wire *dut_dpi_result = wrapper->addWire(ID(dut_dpi_result), dut_result_width);

        // =========================================================================
        // Instantiate AXI Interconnect
        // =========================================================================
        RTLIL::Cell *interconnect = wrapper->addCell(ID(u_interconnect), ID(loom_axi_interconnect));
        interconnect->setParam(ID(ADDR_WIDTH), addr_width);
        interconnect->setPort(ID(clk_i), clk_i);
        interconnect->setPort(ID(rst_ni), rst_ni);
        // Slave interface (from BFM)
        interconnect->setPort(ID(s_axil_araddr_i), s_axil_araddr);
        interconnect->setPort(ID(s_axil_arvalid_i), s_axil_arvalid);
        interconnect->setPort(ID(s_axil_arready_o), s_axil_arready);
        interconnect->setPort(ID(s_axil_rdata_o), s_axil_rdata);
        interconnect->setPort(ID(s_axil_rresp_o), s_axil_rresp);
        interconnect->setPort(ID(s_axil_rvalid_o), s_axil_rvalid);
        interconnect->setPort(ID(s_axil_rready_i), s_axil_rready);
        interconnect->setPort(ID(s_axil_awaddr_i), s_axil_awaddr);
        interconnect->setPort(ID(s_axil_awvalid_i), s_axil_awvalid);
        interconnect->setPort(ID(s_axil_awready_o), s_axil_awready);
        interconnect->setPort(ID(s_axil_wdata_i), s_axil_wdata);
        interconnect->setPort(ID(s_axil_wstrb_i), s_axil_wstrb);
        interconnect->setPort(ID(s_axil_wvalid_i), s_axil_wvalid);
        interconnect->setPort(ID(s_axil_wready_o), s_axil_wready);
        interconnect->setPort(ID(s_axil_bresp_o), s_axil_bresp);
        interconnect->setPort(ID(s_axil_bvalid_o), s_axil_bvalid);
        interconnect->setPort(ID(s_axil_bready_i), s_axil_bready);
        // Master 0 (to emu_ctrl)
        interconnect->setPort(ID(m0_axil_araddr_o), m0_araddr);
        interconnect->setPort(ID(m0_axil_arvalid_o), m0_arvalid);
        interconnect->setPort(ID(m0_axil_arready_i), m0_arready);
        interconnect->setPort(ID(m0_axil_rdata_i), m0_rdata);
        interconnect->setPort(ID(m0_axil_rresp_i), m0_rresp);
        interconnect->setPort(ID(m0_axil_rvalid_i), m0_rvalid);
        interconnect->setPort(ID(m0_axil_rready_o), m0_rready);
        interconnect->setPort(ID(m0_axil_awaddr_o), m0_awaddr);
        interconnect->setPort(ID(m0_axil_awvalid_o), m0_awvalid);
        interconnect->setPort(ID(m0_axil_awready_i), m0_awready);
        interconnect->setPort(ID(m0_axil_wdata_o), m0_wdata);
        interconnect->setPort(ID(m0_axil_wvalid_o), m0_wvalid);
        interconnect->setPort(ID(m0_axil_wready_i), m0_wready);
        interconnect->setPort(ID(m0_axil_bresp_i), m0_bresp);
        interconnect->setPort(ID(m0_axil_bvalid_i), m0_bvalid);
        interconnect->setPort(ID(m0_axil_bready_o), m0_bready);
        // Master 1 (to dpi_regfile)
        interconnect->setPort(ID(m1_axil_araddr_o), m1_araddr);
        interconnect->setPort(ID(m1_axil_arvalid_o), m1_arvalid);
        interconnect->setPort(ID(m1_axil_arready_i), m1_arready);
        interconnect->setPort(ID(m1_axil_rdata_i), m1_rdata);
        interconnect->setPort(ID(m1_axil_rresp_i), m1_rresp);
        interconnect->setPort(ID(m1_axil_rvalid_i), m1_rvalid);
        interconnect->setPort(ID(m1_axil_rready_o), m1_rready);
        interconnect->setPort(ID(m1_axil_awaddr_o), m1_awaddr);
        interconnect->setPort(ID(m1_axil_awvalid_o), m1_awvalid);
        interconnect->setPort(ID(m1_axil_awready_i), m1_awready);
        interconnect->setPort(ID(m1_axil_wdata_o), m1_wdata);
        interconnect->setPort(ID(m1_axil_wvalid_o), m1_wvalid);
        interconnect->setPort(ID(m1_axil_wready_i), m1_wready);
        interconnect->setPort(ID(m1_axil_bresp_i), m1_bresp);
        interconnect->setPort(ID(m1_axil_bvalid_i), m1_bvalid);
        interconnect->setPort(ID(m1_axil_bready_o), m1_bready);
        // Master 2 (to scan_ctrl)
        interconnect->setPort(ID(m2_axil_araddr_o), m2_araddr);
        interconnect->setPort(ID(m2_axil_arvalid_o), m2_arvalid);
        interconnect->setPort(ID(m2_axil_arready_i), m2_arready);
        interconnect->setPort(ID(m2_axil_rdata_i), m2_rdata);
        interconnect->setPort(ID(m2_axil_rresp_i), m2_rresp);
        interconnect->setPort(ID(m2_axil_rvalid_i), m2_rvalid);
        interconnect->setPort(ID(m2_axil_rready_o), m2_rready);
        interconnect->setPort(ID(m2_axil_awaddr_o), m2_awaddr);
        interconnect->setPort(ID(m2_axil_awvalid_o), m2_awvalid);
        interconnect->setPort(ID(m2_axil_awready_i), m2_awready);
        interconnect->setPort(ID(m2_axil_wdata_o), m2_wdata);
        interconnect->setPort(ID(m2_axil_wvalid_o), m2_wvalid);
        interconnect->setPort(ID(m2_axil_wready_i), m2_wready);
        interconnect->setPort(ID(m2_axil_bresp_i), m2_bresp);
        interconnect->setPort(ID(m2_axil_bvalid_i), m2_bvalid);
        interconnect->setPort(ID(m2_axil_bready_o), m2_bready);

        // =========================================================================
        // Instantiate Emulation Controller (includes DPI bridge)
        // =========================================================================
        RTLIL::Cell *emu_ctrl = wrapper->addCell(ID(u_emu_ctrl), ID(loom_emu_ctrl));
        emu_ctrl->setParam(ID(N_DPI_FUNCS), n_dpi);
        emu_ctrl->setParam(ID(N_MEMORIES), 0);
        emu_ctrl->setParam(ID(N_SCAN_CHAINS), 1);
        emu_ctrl->setParam(ID(TOTAL_SCAN_BITS), scan_chain_length);
        emu_ctrl->setParam(ID(MAX_ARG_WIDTH), dut_args_width);
        emu_ctrl->setParam(ID(MAX_RET_WIDTH), dut_result_width);
        emu_ctrl->setParam(ID(MAX_ARGS), max_args);
        emu_ctrl->setParam(ID(DESIGN_ID), 0xE2E00001);
        emu_ctrl->setParam(ID(LOOM_VERSION), 0x000100);
        emu_ctrl->setPort(ID(clk_i), clk_i);
        emu_ctrl->setPort(ID(rst_ni), rst_ni);
        // AXI-Lite
        emu_ctrl->setPort(ID(axil_araddr_i), m0_araddr);
        emu_ctrl->setPort(ID(axil_arvalid_i), m0_arvalid);
        emu_ctrl->setPort(ID(axil_arready_o), m0_arready);
        emu_ctrl->setPort(ID(axil_rdata_o), m0_rdata);
        emu_ctrl->setPort(ID(axil_rresp_o), m0_rresp);
        emu_ctrl->setPort(ID(axil_rvalid_o), m0_rvalid);
        emu_ctrl->setPort(ID(axil_rready_i), m0_rready);
        emu_ctrl->setPort(ID(axil_awaddr_i), m0_awaddr);
        emu_ctrl->setPort(ID(axil_awvalid_i), m0_awvalid);
        emu_ctrl->setPort(ID(axil_awready_o), m0_awready);
        emu_ctrl->setPort(ID(axil_wdata_i), m0_wdata);
        emu_ctrl->setPort(ID(axil_wvalid_i), m0_wvalid);
        emu_ctrl->setPort(ID(axil_wready_o), m0_wready);
        emu_ctrl->setPort(ID(axil_bresp_o), m0_bresp);
        emu_ctrl->setPort(ID(axil_bvalid_o), m0_bvalid);
        emu_ctrl->setPort(ID(axil_bready_i), m0_bready);
        // DUT DPI interface
        emu_ctrl->setPort(ID(dut_dpi_valid_i), dut_dpi_valid_w);
        emu_ctrl->setPort(ID(dut_dpi_func_id_i), dut_dpi_func_id);
        emu_ctrl->setPort(ID(dut_dpi_args_i), dut_dpi_args);
        emu_ctrl->setPort(ID(dut_dpi_result_o), dut_dpi_result);
        emu_ctrl->setPort(ID(dut_dpi_ready_o), dut_dpi_ack);
        // DPI regfile interface
        emu_ctrl->setPort(ID(dpi_call_valid_o), dpi_call_valid);
        emu_ctrl->setPort(ID(dpi_call_ready_i), dpi_call_ready);
        emu_ctrl->setPort(ID(dpi_call_args_o), dpi_call_args);
        emu_ctrl->setPort(ID(dpi_ret_valid_i), dpi_ret_valid);
        emu_ctrl->setPort(ID(dpi_ret_ready_o), dpi_ret_ready);
        emu_ctrl->setPort(ID(dpi_ret_data_i), dpi_ret_data);
        // Finish
        emu_ctrl->setPort(ID(dut_finish_req_i), RTLIL::SigSpec(RTLIL::State::S0));
        emu_ctrl->setPort(ID(dut_finish_code_i), RTLIL::SigSpec(RTLIL::State::S0, 8));
        // Outputs
        emu_ctrl->setPort(ID(loom_en_o), loom_en_wire);
        emu_ctrl->setPort(ID(dut_rst_no), dut_rst_n);
        emu_ctrl->setPort(ID(cycle_count_o), cycle_count);
        emu_ctrl->setPort(ID(finish_o), emu_finish);
        emu_ctrl->setPort(ID(irq_state_change_o), irq_state_change);

        // =========================================================================
        // Instantiate DPI Register File
        // =========================================================================
        RTLIL::Cell *dpi_regfile = wrapper->addCell(ID(u_dpi_regfile), ID(loom_dpi_regfile));
        dpi_regfile->setParam(ID(N_DPI_FUNCS), n_dpi_funcs > 0 ? n_dpi_funcs : 1);
        dpi_regfile->setParam(ID(MAX_ARGS), max_args);
        dpi_regfile->setPort(ID(clk_i), clk_i);
        dpi_regfile->setPort(ID(rst_ni), rst_ni);
        dpi_regfile->setPort(ID(axil_araddr_i), m1_araddr);
        dpi_regfile->setPort(ID(axil_arvalid_i), m1_arvalid);
        dpi_regfile->setPort(ID(axil_arready_o), m1_arready);
        dpi_regfile->setPort(ID(axil_rdata_o), m1_rdata);
        dpi_regfile->setPort(ID(axil_rresp_o), m1_rresp);
        dpi_regfile->setPort(ID(axil_rvalid_o), m1_rvalid);
        dpi_regfile->setPort(ID(axil_rready_i), m1_rready);
        dpi_regfile->setPort(ID(axil_awaddr_i), m1_awaddr);
        dpi_regfile->setPort(ID(axil_awvalid_i), m1_awvalid);
        dpi_regfile->setPort(ID(axil_awready_o), m1_awready);
        dpi_regfile->setPort(ID(axil_wdata_i), m1_wdata);
        dpi_regfile->setPort(ID(axil_wvalid_i), m1_wvalid);
        dpi_regfile->setPort(ID(axil_wready_o), m1_wready);
        dpi_regfile->setPort(ID(axil_bresp_o), m1_bresp);
        dpi_regfile->setPort(ID(axil_bvalid_o), m1_bvalid);
        dpi_regfile->setPort(ID(axil_bready_i), m1_bready);
        dpi_regfile->setPort(ID(dpi_call_valid_i), dpi_call_valid);
        dpi_regfile->setPort(ID(dpi_call_ready_o), dpi_call_ready);
        dpi_regfile->setPort(ID(dpi_call_args_i), dpi_call_args);
        dpi_regfile->setPort(ID(dpi_ret_valid_o), dpi_ret_valid);
        dpi_regfile->setPort(ID(dpi_ret_ready_i), dpi_ret_ready);
        dpi_regfile->setPort(ID(dpi_ret_data_o), dpi_ret_data);
        dpi_regfile->setPort(ID(dpi_stall_o), dpi_stall);

        // =========================================================================
        // Instantiate Scan Controller
        // =========================================================================
        RTLIL::Cell *scan_ctrl = wrapper->addCell(ID(u_scan_ctrl), ID(loom_scan_ctrl));
        scan_ctrl->setParam(ID(CHAIN_LENGTH), scan_chain_length);
        scan_ctrl->setPort(ID(clk_i), clk_i);
        scan_ctrl->setPort(ID(rst_ni), rst_ni);
        scan_ctrl->setPort(ID(axil_araddr_i), m2_araddr);
        scan_ctrl->setPort(ID(axil_arvalid_i), m2_arvalid);
        scan_ctrl->setPort(ID(axil_arready_o), m2_arready);
        scan_ctrl->setPort(ID(axil_rdata_o), m2_rdata);
        scan_ctrl->setPort(ID(axil_rresp_o), m2_rresp);
        scan_ctrl->setPort(ID(axil_rvalid_o), m2_rvalid);
        scan_ctrl->setPort(ID(axil_rready_i), m2_rready);
        scan_ctrl->setPort(ID(axil_awaddr_i), m2_awaddr);
        scan_ctrl->setPort(ID(axil_awvalid_i), m2_awvalid);
        scan_ctrl->setPort(ID(axil_awready_o), m2_awready);
        scan_ctrl->setPort(ID(axil_wdata_i), m2_wdata);
        scan_ctrl->setPort(ID(axil_wvalid_i), m2_wvalid);
        scan_ctrl->setPort(ID(axil_wready_o), m2_wready);
        scan_ctrl->setPort(ID(axil_bresp_o), m2_bresp);
        scan_ctrl->setPort(ID(axil_bvalid_o), m2_bvalid);
        scan_ctrl->setPort(ID(axil_bready_i), m2_bready);
        scan_ctrl->setPort(ID(scan_enable_o), scan_enable);
        scan_ctrl->setPort(ID(scan_in_o), scan_in);
        scan_ctrl->setPort(ID(scan_out_i), scan_out);
        scan_ctrl->setPort(ID(scan_busy_o), scan_busy);

        // =========================================================================
        // Instantiate DUT
        // =========================================================================
        RTLIL::Cell *dut_inst = wrapper->addCell(ID(u_dut), dut->name);

        // Connect DUT ports
        for (auto wire : dut->wires()) {
            if (!wire->port_input && !wire->port_output)
                continue;

            std::string wire_name = wire->name.str();

            // Handle clock - connect to ungated clock (free-running)
            if (wire->name == RTLIL::escape_id(clk_name)) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(clk_i));
                continue;
            }

            // Handle reset - connect to dut_rst_n from emu_ctrl
            if (wire->name == RTLIL::escape_id(rst_name)) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_rst_n));
                continue;
            }

            // Handle loom_en - connect to computed loom_en_wire
            if (wire_name.find("loom_en") != std::string::npos &&
                wire_name.find("loom_en") == wire_name.length() - 7) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(loom_en_wire));
                continue;
            }

            // Handle DPI signals
            if (wire_name.find("loom_dpi_valid") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_dpi_valid_w));
                continue;
            }
            if (wire_name.find("loom_dpi_func_id") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_dpi_func_id));
                continue;
            }
            if (wire_name.find("loom_dpi_args") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_dpi_args));
                continue;
            }
            if (wire_name.find("loom_dpi_result") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_dpi_result));
                continue;
            }
            if (wire_name.find("loom_dpi_ack") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_dpi_ack));
                continue;
            }

            // Handle finish signal from DUT
            if (wire_name.find("loom_finish_o") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(dut_finish));
                continue;
            }

            // Handle scan signals (if present) - connect to scan_ctrl
            if (wire_name.find("loom_scan_enable") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(scan_enable));
                continue;
            }
            if (wire_name.find("loom_scan_in") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(scan_in));
                continue;
            }
            if (wire_name.find("loom_scan_out") != std::string::npos) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(scan_out));
                continue;
            }

            // All other inputs: tie to '0
            if (wire->port_input) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(RTLIL::State::S0, GetSize(wire)));
                log("  Tying DUT input '%s' to '0\n", wire_name.c_str());
            }
            // All other outputs: leave unconnected (connect to dummy wire)
            if (wire->port_output && wire_name.find("loom_") == std::string::npos) {
                RTLIL::Wire *unused = wrapper->addWire(wrapper->uniquify("\\unused_" + wire_name.substr(1)), GetSize(wire));
                dut_inst->setPort(wire->name, RTLIL::SigSpec(unused));
                log("  Leaving DUT output '%s' unconnected\n", wire_name.c_str());
            }
        }

        // =========================================================================
        // IRQ wiring
        // =========================================================================
        RTLIL::Wire *irq_dpi = wrapper->addWire(NEW_ID, 1);
        wrapper->addReduceOr(NEW_ID, RTLIL::SigSpec(dpi_stall), RTLIL::SigSpec(irq_dpi));

        RTLIL::SigSpec irq_sig;
        irq_sig.append(RTLIL::SigSpec(irq_dpi));           // IRQ[0]
        irq_sig.append(RTLIL::SigSpec(irq_state_change));  // IRQ[1]
        irq_sig.append(RTLIL::SigSpec(RTLIL::State::S0, n_irq - 2));  // IRQ[15:2]
        wrapper->connect(RTLIL::SigSpec(irq_o), irq_sig);

        // =========================================================================
        // Finish wiring
        // =========================================================================
        RTLIL::Wire *not_scan_busy = wrapper->addWire(NEW_ID, 1);
        RTLIL::Wire *dut_finish_masked = wrapper->addWire(NEW_ID, 1);
        wrapper->addNot(NEW_ID, RTLIL::SigSpec(scan_busy), RTLIL::SigSpec(not_scan_busy));
        wrapper->addAnd(NEW_ID, RTLIL::SigSpec(dut_finish), RTLIL::SigSpec(not_scan_busy), RTLIL::SigSpec(dut_finish_masked));
        RTLIL::Wire *combined_finish = wrapper->addWire(NEW_ID, 1);
        wrapper->addOr(NEW_ID, RTLIL::SigSpec(emu_finish), RTLIL::SigSpec(dut_finish_masked), RTLIL::SigSpec(combined_finish));
        wrapper->connect(RTLIL::SigSpec(finish_o), RTLIL::SigSpec(combined_finish));

        wrapper->fixup_ports();

        log("Generated loom_emu_top module\n");
        log("  Instantiated: loom_axi_interconnect (u_interconnect)\n");
        log("  Instantiated: loom_emu_ctrl (u_emu_ctrl) - controls loom_en + DPI bridge\n");
        log("  Instantiated: loom_dpi_regfile (u_dpi_regfile)\n");
        log("  Instantiated: loom_scan_ctrl (u_scan_ctrl) - %d bits\n", scan_chain_length);
        log("  Instantiated: %s (u_dut) - clock free-running, loom_en for FF enable\n", top_name.c_str());
    }
};

EmuTopPass EmuTopPass_singleton;

PRIVATE_NAMESPACE_END
