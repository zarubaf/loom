// SPDX-License-Identifier: Apache-2.0
/*
 * emu_top - Yosys pass for complete emulation wrapper generation
 *
 * This pass generates a complete emulation top-level module `loom_emu_top` that includes:
 *   1. The transformed DUT (after loom_instrument pass)
 *   2. loom_emu_ctrl: emulation controller + DPI bridge
 *   3. loom_axil_demux: routes AXI-Lite to ctrl, regfile, and scan
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

        // Auto-detect memory shadow from mem_shadow attributes
        int n_memories = 0;
        int shadow_addr_bits = 0;
        int shadow_data_bits = 0;
        uint32_t shadow_total_bytes = 0;
        std::string n_mem_str = dut->get_string_attribute(ID(loom_n_memories));
        if (!n_mem_str.empty()) {
            n_memories = atoi(n_mem_str.c_str());
            std::string s;
            s = dut->get_string_attribute(ID(loom_shadow_addr_bits));
            if (!s.empty()) shadow_addr_bits = atoi(s.c_str());
            s = dut->get_string_attribute(ID(loom_shadow_data_bits));
            if (!s.empty()) shadow_data_bits = atoi(s.c_str());
            s = dut->get_string_attribute(ID(loom_shadow_total_bytes));
            if (!s.empty()) shadow_total_bytes = atoi(s.c_str());
        }
        bool has_memories = (n_memories > 0);

        // Auto-detect clock name from tbx clkgen detection
        std::string tbx_clk = dut->get_string_attribute(RTLIL::IdString("\\loom_tbx_clk"));
        if (!tbx_clk.empty()) {
            if (clk_name == "clk_i") {
                // Default clock name — override with detected
                clk_name = tbx_clk;
                log("  Clock from tbx clkgen: %s\n", tbx_clk.c_str());
            } else if (clk_name != tbx_clk) {
                log_error("Clock name conflict: -clk specifies '%s' but "
                          "tbx clkgen detected '%s'.\n"
                          "Remove -clk to use the auto-detected clock, "
                          "or fix the mismatch.\n",
                          clk_name.c_str(), tbx_clk.c_str());
            }
        }

        bool resets_extracted = dut->get_bool_attribute(ID(loom_resets_extracted));

        // Ensure clock (and reset, if not extracted) are input ports on the DUT.
        // This handles the "tbx clkgen" pattern where the clock is driven by
        // an initial block (skipped by --ignore-initial) and may have been
        // optimized away or left as an internal wire.
        std::vector<std::string> ensure_ports = {clk_name};
        if (!resets_extracted)
            ensure_ports.push_back(rst_name);
        for (auto &sig_name : ensure_ports) {
            RTLIL::Wire *w = dut->wire(RTLIL::escape_id(sig_name));
            if (!w) {
                // Wire was optimized away — recreate as input port
                log("  Creating input port '%s' (was missing — tbx clkgen pattern)\n", sig_name.c_str());
                w = dut->addWire(RTLIL::escape_id(sig_name), 1);
                w->port_input = true;
                dut->fixup_ports();
            } else if (!w->port_input) {
                log("  Promoting internal wire '%s' to input port\n", sig_name.c_str());
                w->port_input = true;
                dut->fixup_ports();
            }
        }

        log("Creating loom_emu_top wrapper for DUT '%s'\n", top_name.c_str());
        log("  Clock: %s, Reset: %s\n", clk_name.c_str(), rst_name.c_str());
        log("  DPI functions: %d (auto-detected)\n", n_dpi_funcs);
        log("  Scan chain: %d bits (auto-detected)\n", scan_chain_length);
        log("  Memories: %d (auto-detected)\n", n_memories);

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
        // Create internal wires for demux master ports (flat arrays)
        // =========================================================================
        const int n_demux_masters = has_memories ? 4 : 3;

        // Flat bus wires for demux
        RTLIL::Wire *demux_araddr  = wrapper->addWire(ID(demux_araddr),  n_demux_masters * addr_width);
        RTLIL::Wire *demux_arvalid = wrapper->addWire(ID(demux_arvalid), n_demux_masters);
        RTLIL::Wire *demux_arready = wrapper->addWire(ID(demux_arready), n_demux_masters);
        RTLIL::Wire *demux_rdata   = wrapper->addWire(ID(demux_rdata),   n_demux_masters * 32);
        RTLIL::Wire *demux_rresp   = wrapper->addWire(ID(demux_rresp),   n_demux_masters * 2);
        RTLIL::Wire *demux_rvalid  = wrapper->addWire(ID(demux_rvalid),  n_demux_masters);
        RTLIL::Wire *demux_rready  = wrapper->addWire(ID(demux_rready),  n_demux_masters);

        RTLIL::Wire *demux_awaddr  = wrapper->addWire(ID(demux_awaddr),  n_demux_masters * addr_width);
        RTLIL::Wire *demux_awvalid = wrapper->addWire(ID(demux_awvalid), n_demux_masters);
        RTLIL::Wire *demux_awready = wrapper->addWire(ID(demux_awready), n_demux_masters);
        RTLIL::Wire *demux_wdata   = wrapper->addWire(ID(demux_wdata),   n_demux_masters * 32);
        RTLIL::Wire *demux_wstrb   = wrapper->addWire(ID(demux_wstrb),   n_demux_masters * 4);
        RTLIL::Wire *demux_wvalid  = wrapper->addWire(ID(demux_wvalid),  n_demux_masters);
        RTLIL::Wire *demux_wready  = wrapper->addWire(ID(demux_wready),  n_demux_masters);
        RTLIL::Wire *demux_bresp   = wrapper->addWire(ID(demux_bresp),   n_demux_masters * 2);
        RTLIL::Wire *demux_bvalid  = wrapper->addWire(ID(demux_bvalid),  n_demux_masters);
        RTLIL::Wire *demux_bready  = wrapper->addWire(ID(demux_bready),  n_demux_masters);

        // Helper lambda: extract a slice from a flat bus wire
        auto slice = [&](RTLIL::Wire *bus, int idx, int width) -> RTLIL::SigSpec {
            return RTLIL::SigSpec(bus, idx * width, width);
        };
        auto bit = [&](RTLIL::Wire *bus, int idx) -> RTLIL::SigSpec {
            return RTLIL::SigSpec(bus, idx, 1);
        };

        // Scan chain signals
        RTLIL::Wire *scan_enable = wrapper->addWire(ID(scan_enable), 1);
        RTLIL::Wire *scan_in = wrapper->addWire(ID(scan_in), 1);
        RTLIL::Wire *scan_out = wrapper->addWire(ID(scan_out), 1);
        RTLIL::Wire *scan_busy = wrapper->addWire(ID(scan_busy), 1);

        // emu_ctrl signals
        RTLIL::Wire *loom_en_wire = wrapper->addWire(ID(loom_en_wire), 1);
        RTLIL::Wire *cycle_count = wrapper->addWire(ID(cycle_count), 64);
        RTLIL::Wire *irq_state_change = wrapper->addWire(ID(irq_state_change), 1);
        RTLIL::Wire *emu_finish = wrapper->addWire(ID(emu_finish), 1);
        RTLIL::Wire *dut_finish = wrapper->addWire(ID(dut_finish), 1);

        // Read actual DPI port widths from DUT (set by loom_instrument)
        int dut_args_width = 64;   // default
        int dut_result_width = 32; // default
        for (auto wire : dut->wires()) {
            std::string wn = wire->name.str();
            if (wn.find("loom_dpi_args") != std::string::npos && wire->port_output)
                dut_args_width = wire->width;
            if (wn.find("loom_dpi_result") != std::string::npos && wire->port_input)
                dut_result_width = wire->width;
        }

        // DPI regfile <-> emu_ctrl signals
        // Compute max_args from BOTH the input args port and the output args
        // portion of the result port, since the regfile arg registers serve
        // both directions (DUT→host on call, host→DUT on completion).
        // The 64-byte per-function block has room for status(1) + control(1) + args(N) +
        // result(2) = N+4 registers, so N <= 12 with the current address decode scheme.
        int input_arg_words = (dut_args_width + 31) / 32;
        int output_arg_words = (dut_result_width > 64) ? (dut_result_width - 64 + 31) / 32 : 0;
        int max_args = std::max(input_arg_words, output_arg_words);
        if (max_args < 1) max_args = 1;
        if (max_args > 12)
            log_error("DPI args width %d bits (%d words) exceeds 12-word regfile limit.\n"
                      "Reduce DPI argument sizes or split into multiple calls.\n",
                      dut_args_width, max_args);
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

        // DUT DPI interface wires (connected through emu_ctrl)
        RTLIL::Wire *dut_dpi_valid_w = wrapper->addWire(ID(dut_dpi_valid), 1);
        RTLIL::Wire *dut_dpi_ack = wrapper->addWire(ID(dut_dpi_ack), 1);
        RTLIL::Wire *dut_dpi_func_id = wrapper->addWire(ID(dut_dpi_func_id), 8);
        RTLIL::Wire *dut_dpi_args = wrapper->addWire(ID(dut_dpi_args), dut_args_width);
        RTLIL::Wire *dut_dpi_result = wrapper->addWire(ID(dut_dpi_result), dut_result_width);

        // =========================================================================
        // Instantiate AXI-Lite Demux
        // =========================================================================
        // Address map: slave 0 = emu_ctrl  (0x00000, mask 0xF0000)
        //              slave 1 = dpi_regfile (0x10000, mask 0xF0000)
        //              slave 2 = scan_ctrl   (0x20000, mask 0xF0000)
        RTLIL::Cell *interconnect = wrapper->addCell(ID(u_interconnect), ID(loom_axil_demux));
        interconnect->setParam(ID(ADDR_WIDTH), addr_width);
        interconnect->setParam(ID(N_MASTERS), n_demux_masters);

        // BASE_ADDR: packed [N-1:0][AW-1:0]
        //   Master 0: ctrl     = 0x00000
        //   Master 1: dpi      = 0x10000
        //   Master 2: scan     = 0x20000
        //   Master 3: mem_ctrl = 0x30000 (when present)
        RTLIL::Const base_addr_val(0, n_demux_masters * addr_width);
        // Master 0: BASE=0x00000 (already zero)
        // Master 1: BASE=0x10000
        base_addr_val.bits()[1 * addr_width + 16] = RTLIL::State::S1; // bit 16 of master 1
        // Master 2: BASE=0x20000
        base_addr_val.bits()[2 * addr_width + 17] = RTLIL::State::S1; // bit 17 of master 2
        // Master 3: BASE=0x30000
        if (has_memories) {
            base_addr_val.bits()[3 * addr_width + 16] = RTLIL::State::S1; // bit 16
            base_addr_val.bits()[3 * addr_width + 17] = RTLIL::State::S1; // bit 17
        }
        interconnect->setParam(ID(BASE_ADDR), base_addr_val);

        // ADDR_MASK: all slaves use 0xF0000 (bits [19:16])
        RTLIL::Const addr_mask_val(0, n_demux_masters * addr_width);
        for (int m = 0; m < n_demux_masters; m++)
            for (int b = 16; b < addr_width; b++)
                addr_mask_val.bits()[m * addr_width + b] = RTLIL::State::S1;
        interconnect->setParam(ID(ADDR_MASK), addr_mask_val);

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
        // Master ports (flat arrays)
        interconnect->setPort(ID(m_axil_araddr_o), demux_araddr);
        interconnect->setPort(ID(m_axil_arvalid_o), demux_arvalid);
        interconnect->setPort(ID(m_axil_arready_i), demux_arready);
        interconnect->setPort(ID(m_axil_rdata_i), demux_rdata);
        interconnect->setPort(ID(m_axil_rresp_i), demux_rresp);
        interconnect->setPort(ID(m_axil_rvalid_i), demux_rvalid);
        interconnect->setPort(ID(m_axil_rready_o), demux_rready);
        interconnect->setPort(ID(m_axil_awaddr_o), demux_awaddr);
        interconnect->setPort(ID(m_axil_awvalid_o), demux_awvalid);
        interconnect->setPort(ID(m_axil_awready_i), demux_awready);
        interconnect->setPort(ID(m_axil_wdata_o), demux_wdata);
        interconnect->setPort(ID(m_axil_wstrb_o), demux_wstrb);
        interconnect->setPort(ID(m_axil_wvalid_o), demux_wvalid);
        interconnect->setPort(ID(m_axil_wready_i), demux_wready);
        interconnect->setPort(ID(m_axil_bresp_i), demux_bresp);
        interconnect->setPort(ID(m_axil_bvalid_i), demux_bvalid);
        interconnect->setPort(ID(m_axil_bready_o), demux_bready);

        // =========================================================================
        // Instantiate Emulation Controller (includes DPI bridge)
        // =========================================================================
        RTLIL::Cell *emu_ctrl = wrapper->addCell(ID(u_emu_ctrl), ID(loom_emu_ctrl));
        emu_ctrl->setParam(ID(N_DPI_FUNCS), n_dpi);
        emu_ctrl->setParam(ID(N_MEMORIES), n_memories);
        emu_ctrl->setParam(ID(N_SCAN_CHAINS), 1);
        emu_ctrl->setParam(ID(TOTAL_SCAN_BITS), scan_chain_length);
        emu_ctrl->setParam(ID(MAX_ARG_WIDTH), dut_args_width);
        emu_ctrl->setParam(ID(MAX_RET_WIDTH), dut_result_width);
        emu_ctrl->setParam(ID(MAX_ARGS), max_args);
        emu_ctrl->setParam(ID(DESIGN_ID), 0xE2E00001);
        emu_ctrl->setParam(ID(LOOM_VERSION), 0x000100);
        emu_ctrl->setPort(ID(clk_i), clk_i);
        emu_ctrl->setPort(ID(rst_ni), rst_ni);
        // AXI-Lite (from demux master 0)
        emu_ctrl->setPort(ID(axil_araddr_i), slice(demux_araddr, 0, addr_width));
        emu_ctrl->setPort(ID(axil_arvalid_i), bit(demux_arvalid, 0));
        emu_ctrl->setPort(ID(axil_arready_o), bit(demux_arready, 0));
        emu_ctrl->setPort(ID(axil_rdata_o), slice(demux_rdata, 0, 32));
        emu_ctrl->setPort(ID(axil_rresp_o), slice(demux_rresp, 0, 2));
        emu_ctrl->setPort(ID(axil_rvalid_o), bit(demux_rvalid, 0));
        emu_ctrl->setPort(ID(axil_rready_i), bit(demux_rready, 0));
        emu_ctrl->setPort(ID(axil_awaddr_i), slice(demux_awaddr, 0, addr_width));
        emu_ctrl->setPort(ID(axil_awvalid_i), bit(demux_awvalid, 0));
        emu_ctrl->setPort(ID(axil_awready_o), bit(demux_awready, 0));
        emu_ctrl->setPort(ID(axil_wdata_i), slice(demux_wdata, 0, 32));
        emu_ctrl->setPort(ID(axil_wvalid_i), bit(demux_wvalid, 0));
        emu_ctrl->setPort(ID(axil_wready_o), bit(demux_wready, 0));
        emu_ctrl->setPort(ID(axil_bresp_o), slice(demux_bresp, 0, 2));
        emu_ctrl->setPort(ID(axil_bvalid_o), bit(demux_bvalid, 0));
        emu_ctrl->setPort(ID(axil_bready_i), bit(demux_bready, 0));
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
        dpi_regfile->setPort(ID(axil_araddr_i), slice(demux_araddr, 1, addr_width));
        dpi_regfile->setPort(ID(axil_arvalid_i), bit(demux_arvalid, 1));
        dpi_regfile->setPort(ID(axil_arready_o), bit(demux_arready, 1));
        dpi_regfile->setPort(ID(axil_rdata_o), slice(demux_rdata, 1, 32));
        dpi_regfile->setPort(ID(axil_rresp_o), slice(demux_rresp, 1, 2));
        dpi_regfile->setPort(ID(axil_rvalid_o), bit(demux_rvalid, 1));
        dpi_regfile->setPort(ID(axil_rready_i), bit(demux_rready, 1));
        dpi_regfile->setPort(ID(axil_awaddr_i), slice(demux_awaddr, 1, addr_width));
        dpi_regfile->setPort(ID(axil_awvalid_i), bit(demux_awvalid, 1));
        dpi_regfile->setPort(ID(axil_awready_o), bit(demux_awready, 1));
        dpi_regfile->setPort(ID(axil_wdata_i), slice(demux_wdata, 1, 32));
        dpi_regfile->setPort(ID(axil_wvalid_i), bit(demux_wvalid, 1));
        dpi_regfile->setPort(ID(axil_wready_o), bit(demux_wready, 1));
        dpi_regfile->setPort(ID(axil_bresp_o), slice(demux_bresp, 1, 2));
        dpi_regfile->setPort(ID(axil_bvalid_o), bit(demux_bvalid, 1));
        dpi_regfile->setPort(ID(axil_bready_i), bit(demux_bready, 1));
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
        scan_ctrl->setPort(ID(axil_araddr_i), slice(demux_araddr, 2, addr_width));
        scan_ctrl->setPort(ID(axil_arvalid_i), bit(demux_arvalid, 2));
        scan_ctrl->setPort(ID(axil_arready_o), bit(demux_arready, 2));
        scan_ctrl->setPort(ID(axil_rdata_o), slice(demux_rdata, 2, 32));
        scan_ctrl->setPort(ID(axil_rresp_o), slice(demux_rresp, 2, 2));
        scan_ctrl->setPort(ID(axil_rvalid_o), bit(demux_rvalid, 2));
        scan_ctrl->setPort(ID(axil_rready_i), bit(demux_rready, 2));
        scan_ctrl->setPort(ID(axil_awaddr_i), slice(demux_awaddr, 2, addr_width));
        scan_ctrl->setPort(ID(axil_awvalid_i), bit(demux_awvalid, 2));
        scan_ctrl->setPort(ID(axil_awready_o), bit(demux_awready, 2));
        scan_ctrl->setPort(ID(axil_wdata_i), slice(demux_wdata, 2, 32));
        scan_ctrl->setPort(ID(axil_wvalid_i), bit(demux_wvalid, 2));
        scan_ctrl->setPort(ID(axil_wready_o), bit(demux_wready, 2));
        scan_ctrl->setPort(ID(axil_bresp_o), slice(demux_bresp, 2, 2));
        scan_ctrl->setPort(ID(axil_bvalid_o), bit(demux_bvalid, 2));
        scan_ctrl->setPort(ID(axil_bready_i), bit(demux_bready, 2));
        scan_ctrl->setPort(ID(scan_enable_o), scan_enable);
        scan_ctrl->setPort(ID(scan_in_o), scan_in);
        scan_ctrl->setPort(ID(scan_out_i), scan_out);
        scan_ctrl->setPort(ID(scan_busy_o), scan_busy);

        // =========================================================================
        // Conditionally instantiate Memory Controller (4th AXI-Lite slave)
        // =========================================================================
        RTLIL::Wire *shadow_addr_w = nullptr;
        RTLIL::Wire *shadow_wdata_w = nullptr;
        RTLIL::Wire *shadow_rdata_w = nullptr;
        RTLIL::Wire *shadow_wen_w = nullptr;
        RTLIL::Wire *shadow_ren_w = nullptr;

        if (has_memories) {
            shadow_addr_w  = wrapper->addWire(ID(shadow_addr), shadow_addr_bits);
            shadow_wdata_w = wrapper->addWire(ID(shadow_wdata), shadow_data_bits);
            shadow_rdata_w = wrapper->addWire(ID(shadow_rdata), shadow_data_bits);
            shadow_wen_w   = wrapper->addWire(ID(shadow_wen), 1);
            shadow_ren_w   = wrapper->addWire(ID(shadow_ren), 1);

            RTLIL::Cell *mem_ctrl = wrapper->addCell(ID(u_mem_ctrl), ID(loom_mem_ctrl));
            mem_ctrl->setParam(ID(ADDR_BITS), shadow_addr_bits);
            mem_ctrl->setParam(ID(DATA_BITS), shadow_data_bits);
            mem_ctrl->setParam(ID(TOTAL_BYTES), (int)shadow_total_bytes);
            mem_ctrl->setPort(ID(clk_i), clk_i);
            mem_ctrl->setPort(ID(rst_ni), rst_ni);
            mem_ctrl->setPort(ID(axil_araddr_i), slice(demux_araddr, 3, addr_width));
            mem_ctrl->setPort(ID(axil_arvalid_i), bit(demux_arvalid, 3));
            mem_ctrl->setPort(ID(axil_arready_o), bit(demux_arready, 3));
            mem_ctrl->setPort(ID(axil_rdata_o), slice(demux_rdata, 3, 32));
            mem_ctrl->setPort(ID(axil_rresp_o), slice(demux_rresp, 3, 2));
            mem_ctrl->setPort(ID(axil_rvalid_o), bit(demux_rvalid, 3));
            mem_ctrl->setPort(ID(axil_rready_i), bit(demux_rready, 3));
            mem_ctrl->setPort(ID(axil_awaddr_i), slice(demux_awaddr, 3, addr_width));
            mem_ctrl->setPort(ID(axil_awvalid_i), bit(demux_awvalid, 3));
            mem_ctrl->setPort(ID(axil_awready_o), bit(demux_awready, 3));
            mem_ctrl->setPort(ID(axil_wdata_i), slice(demux_wdata, 3, 32));
            mem_ctrl->setPort(ID(axil_wvalid_i), bit(demux_wvalid, 3));
            mem_ctrl->setPort(ID(axil_wready_o), bit(demux_wready, 3));
            mem_ctrl->setPort(ID(axil_bresp_o), slice(demux_bresp, 3, 2));
            mem_ctrl->setPort(ID(axil_bvalid_o), bit(demux_bvalid, 3));
            mem_ctrl->setPort(ID(axil_bready_i), bit(demux_bready, 3));
            mem_ctrl->setPort(ID(shadow_addr_o), shadow_addr_w);
            mem_ctrl->setPort(ID(shadow_wdata_o), shadow_wdata_w);
            mem_ctrl->setPort(ID(shadow_rdata_i), shadow_rdata_w);
            mem_ctrl->setPort(ID(shadow_wen_o), shadow_wen_w);
            mem_ctrl->setPort(ID(shadow_ren_o), shadow_ren_w);
        }

        // =========================================================================
        // Instantiate DUT
        // =========================================================================
        RTLIL::Cell *dut_inst = wrapper->addCell(ID(u_dut), dut->name);

        // Connect DUT ports
        bool dut_has_finish = false;
        bool dut_has_dpi = false;
        for (auto wire : dut->wires()) {
            if (!wire->port_input && !wire->port_output)
                continue;

            std::string wire_name = wire->name.str();

            // Handle clock - connect to ungated clock (free-running)
            if (wire->name == RTLIL::escape_id(clk_name)) {
                dut_inst->setPort(wire->name, RTLIL::SigSpec(clk_i));
                continue;
            }

            // Reset port should have been removed by reset_extract
            if (wire->name == RTLIL::escape_id(rst_name)) {
                log_error("DUT still has reset port '%s' — reset_extract must run before emu_top.\n",
                          rst_name.c_str());
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
                dut_has_dpi = true;
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
                dut_has_finish = true;
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

            // Handle shadow memory ports — wire to mem_ctrl or tie to zero
            if (wire_name.find("loom_shadow_addr") != std::string::npos && wire->port_input) {
                if (has_memories && shadow_addr_w) {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(shadow_addr_w));
                } else {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(RTLIL::State::S0, GetSize(wire)));
                }
                continue;
            }
            if (wire_name.find("loom_shadow_wdata") != std::string::npos && wire->port_input) {
                if (has_memories && shadow_wdata_w) {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(shadow_wdata_w));
                } else {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(RTLIL::State::S0, GetSize(wire)));
                }
                continue;
            }
            if (wire_name.find("loom_shadow_rdata") != std::string::npos && wire->port_output) {
                if (has_memories && shadow_rdata_w) {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(shadow_rdata_w));
                } else {
                    RTLIL::Wire *unused = wrapper->addWire(wrapper->uniquify("\\unused_shadow_rdata"), GetSize(wire));
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(unused));
                }
                continue;
            }
            if (wire_name.find("loom_shadow_wen") != std::string::npos && wire->port_input) {
                if (has_memories && shadow_wen_w) {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(shadow_wen_w));
                } else {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(RTLIL::State::S0, GetSize(wire)));
                }
                continue;
            }
            if (wire_name.find("loom_shadow_ren") != std::string::npos && wire->port_input) {
                if (has_memories && shadow_ren_w) {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(shadow_ren_w));
                } else {
                    dut_inst->setPort(wire->name, RTLIL::SigSpec(RTLIL::State::S0, GetSize(wire)));
                }
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

        // Tie off undriven DUT output signals (prevents random init issues)
        if (!dut_has_finish) {
            wrapper->connect(RTLIL::SigSpec(dut_finish), RTLIL::SigSpec(RTLIL::State::S0));
        }
        if (!dut_has_dpi) {
            wrapper->connect(RTLIL::SigSpec(dut_dpi_valid_w), RTLIL::SigSpec(RTLIL::State::S0));
            wrapper->connect(RTLIL::SigSpec(dut_dpi_func_id), RTLIL::SigSpec(RTLIL::State::S0, 8));
            wrapper->connect(RTLIL::SigSpec(dut_dpi_args), RTLIL::SigSpec(RTLIL::State::S0, dut_args_width));
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
        // Finish wiring — gate DUT finish by loom_en so that combinational
        // DUT outputs based on random/uninitialized FFs (before scan-in)
        // cannot trigger a spurious shutdown.
        // =========================================================================
        RTLIL::Wire *not_scan_busy = wrapper->addWire(NEW_ID, 1);
        RTLIL::Wire *dut_finish_gated = wrapper->addWire(NEW_ID, 1);
        RTLIL::Wire *dut_finish_masked = wrapper->addWire(NEW_ID, 1);
        wrapper->addNot(NEW_ID, RTLIL::SigSpec(scan_busy), RTLIL::SigSpec(not_scan_busy));
        wrapper->addAnd(NEW_ID, RTLIL::SigSpec(dut_finish), RTLIL::SigSpec(loom_en_wire), RTLIL::SigSpec(dut_finish_gated));
        wrapper->addAnd(NEW_ID, RTLIL::SigSpec(dut_finish_gated), RTLIL::SigSpec(not_scan_busy), RTLIL::SigSpec(dut_finish_masked));
        RTLIL::Wire *combined_finish = wrapper->addWire(NEW_ID, 1);
        wrapper->addOr(NEW_ID, RTLIL::SigSpec(emu_finish), RTLIL::SigSpec(dut_finish_masked), RTLIL::SigSpec(combined_finish));
        wrapper->connect(RTLIL::SigSpec(finish_o), RTLIL::SigSpec(combined_finish));

        wrapper->fixup_ports();

        log("Generated loom_emu_top module\n");
        log("  Instantiated: loom_axil_demux (u_interconnect) - %d masters\n", n_demux_masters);
        log("  Instantiated: loom_emu_ctrl (u_emu_ctrl) - controls loom_en + DPI bridge\n");
        log("  Instantiated: loom_dpi_regfile (u_dpi_regfile)\n");
        log("  Instantiated: loom_scan_ctrl (u_scan_ctrl) - %d bits\n", scan_chain_length);
        if (has_memories)
            log("  Instantiated: loom_mem_ctrl (u_mem_ctrl) - %d memories, %u bytes\n",
                n_memories, shadow_total_bytes);
        log("  Instantiated: %s (u_dut) - clock free-running, loom_en for FF enable\n", top_name.c_str());
    }
};

EmuTopPass EmuTopPass_singleton;

PRIVATE_NAMESPACE_END
