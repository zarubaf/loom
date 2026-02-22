// SPDX-License-Identifier: Apache-2.0
// Standalone testbench for loom_axil_firewall
//
// Architecture:
//   fw_test_driver.c ──(unix socket)──► loom_axil_socket_bfm
//                                             │
//                                       loom_axil_demux (N=3)
//                                        /      │       \
//                                      M0       M1       M2
//                                       │        │        │
//                                  fw.s_axi  fw.s_mgmt  slave.ctrl
//                                       │                  │
//                                  fw.m_axi ──────►  slave.data

`timescale 1ns / 1ps

module tb_axil_firewall;

    localparam int ADDR_WIDTH = 20;
    localparam int N_MASTERS  = 3;

    // =====================================================================
    // Clock and Reset
    // =====================================================================
    logic clk;
    logic rst_n;

    initial begin
        clk = 1'b0;
        forever #5 clk = ~clk;  // 100 MHz
    end

    initial begin
        rst_n = 1'b0;
        #100;
        rst_n = 1'b1;
    end

    // =====================================================================
    // Watchdog
    // =====================================================================
    longint unsigned timeout_ns;

    initial begin
        if (!$value$plusargs("timeout=%d", timeout_ns))
            timeout_ns = 50_000_000;  // 50ms default
        #(timeout_ns);
        $display("ERROR: Watchdog timeout after %0d ns", timeout_ns);
        $finish;
    end

    // =====================================================================
    // Wave dump
    // =====================================================================
`ifdef VERILATOR
    initial begin
        $dumpfile("dump.fst");
        $dumpvars(0, tb_axil_firewall);
    end
`endif

    // =====================================================================
    // BFM ↔ Demux Wires
    // =====================================================================
    logic [ADDR_WIDTH-1:0] bfm_araddr,  bfm_awaddr;
    logic                  bfm_arvalid, bfm_awvalid;
    logic                  bfm_arready, bfm_awready;
    logic [31:0]           bfm_rdata,   bfm_wdata;
    logic [1:0]            bfm_rresp,   bfm_bresp;
    logic                  bfm_rvalid,  bfm_bvalid;
    logic                  bfm_rready,  bfm_bready;
    logic [3:0]            bfm_wstrb;
    logic                  bfm_wvalid,  bfm_wready;

    // Demux master-side flat buses
    logic [N_MASTERS*ADDR_WIDTH-1:0] dm_araddr,  dm_awaddr;
    logic [N_MASTERS-1:0]            dm_arvalid, dm_awvalid;
    logic [N_MASTERS-1:0]            dm_arready, dm_awready;
    logic [N_MASTERS*32-1:0]         dm_rdata,   dm_wdata;
    logic [N_MASTERS*2-1:0]          dm_rresp,   dm_bresp;
    logic [N_MASTERS-1:0]            dm_rvalid,  dm_bvalid;
    logic [N_MASTERS-1:0]            dm_rready,  dm_bready;
    logic [N_MASTERS*4-1:0]          dm_wstrb;
    logic [N_MASTERS-1:0]            dm_wvalid,  dm_wready;

    // Firewall ↔ slave wires (downstream AXI)
    logic [ADDR_WIDTH-1:0] fw_m_araddr,  fw_m_awaddr;
    logic [2:0]            fw_m_arprot,  fw_m_awprot;
    logic                  fw_m_arvalid, fw_m_awvalid;
    logic                  fw_m_arready, fw_m_awready;
    logic [31:0]           fw_m_rdata,   fw_m_wdata;
    logic [3:0]            fw_m_wstrb;
    logic [1:0]            fw_m_rresp,   fw_m_bresp;
    logic                  fw_m_rvalid,  fw_m_bvalid;
    logic                  fw_m_rready,  fw_m_bready;
    logic                  fw_m_wvalid,  fw_m_wready;

    // IRQ and shutdown
    logic        fw_irq;
    logic        slave_quit;
    logic        bfm_shutdown;

    // =====================================================================
    // BFM Instantiation
    // =====================================================================
    loom_axil_socket_bfm #(
        .ADDR_WIDTH (ADDR_WIDTH),
        .N_IRQ      (1)
    ) u_bfm (
        .clk_i              (clk),
        .rst_ni             (rst_n),

        .m_axil_araddr_o    (bfm_araddr),
        .m_axil_arvalid_o   (bfm_arvalid),
        .m_axil_arready_i   (bfm_arready),
        .m_axil_rdata_i     (bfm_rdata),
        .m_axil_rresp_i     (bfm_rresp),
        .m_axil_rvalid_i    (bfm_rvalid),
        .m_axil_rready_o    (bfm_rready),

        .m_axil_awaddr_o    (bfm_awaddr),
        .m_axil_awvalid_o   (bfm_awvalid),
        .m_axil_awready_i   (bfm_awready),
        .m_axil_wdata_o     (bfm_wdata),
        .m_axil_wstrb_o     (bfm_wstrb),
        .m_axil_wvalid_o    (bfm_wvalid),
        .m_axil_wready_i    (bfm_wready),
        .m_axil_bresp_i     (bfm_bresp),
        .m_axil_bvalid_i    (bfm_bvalid),
        .m_axil_bready_o    (bfm_bready),

        .irq_i              (fw_irq),
        .finish_i           (slave_quit),
        .shutdown_o         (bfm_shutdown)
    );

    // =====================================================================
    // Shutdown Logic (same pattern as xlnx_xdma BFM)
    // =====================================================================
    logic [3:0] shutdown_cnt;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            shutdown_cnt <= 4'd0;
        end else if (bfm_shutdown) begin
            shutdown_cnt <= shutdown_cnt + 4'd1;
            if (shutdown_cnt == 4'd3) begin
                $display("[TB] Shutdown complete");
                $finish;
            end
        end
    end

    // =====================================================================
    // Demux Instantiation
    // =====================================================================
    loom_axil_demux #(
        .ADDR_WIDTH (ADDR_WIDTH),
        .N_MASTERS  (N_MASTERS),
        .BASE_ADDR  ({20'h20000, 20'h10000, 20'h00000}),
        .ADDR_MASK  ({20'hF0000, 20'hF0000, 20'hF0000})
    ) u_demux (
        .clk_i              (clk),
        .rst_ni             (rst_n),

        // Slave port (from BFM)
        .s_axil_araddr_i    (bfm_araddr),
        .s_axil_arvalid_i   (bfm_arvalid),
        .s_axil_arready_o   (bfm_arready),
        .s_axil_rdata_o     (bfm_rdata),
        .s_axil_rresp_o     (bfm_rresp),
        .s_axil_rvalid_o    (bfm_rvalid),
        .s_axil_rready_i    (bfm_rready),

        .s_axil_awaddr_i    (bfm_awaddr),
        .s_axil_awvalid_i   (bfm_awvalid),
        .s_axil_awready_o   (bfm_awready),
        .s_axil_wdata_i     (bfm_wdata),
        .s_axil_wstrb_i     (bfm_wstrb),
        .s_axil_wvalid_i    (bfm_wvalid),
        .s_axil_wready_o    (bfm_wready),
        .s_axil_bresp_o     (bfm_bresp),
        .s_axil_bvalid_o    (bfm_bvalid),
        .s_axil_bready_i    (bfm_bready),

        // Master ports (flat arrays)
        .m_axil_araddr_o    (dm_araddr),
        .m_axil_arvalid_o   (dm_arvalid),
        .m_axil_arready_i   (dm_arready),
        .m_axil_rdata_i     (dm_rdata),
        .m_axil_rresp_i     (dm_rresp),
        .m_axil_rvalid_i    (dm_rvalid),
        .m_axil_rready_o    (dm_rready),

        .m_axil_awaddr_o    (dm_awaddr),
        .m_axil_awvalid_o   (dm_awvalid),
        .m_axil_awready_i   (dm_awready),
        .m_axil_wdata_o     (dm_wdata),
        .m_axil_wstrb_o     (dm_wstrb),
        .m_axil_wvalid_o    (dm_wvalid),
        .m_axil_wready_i    (dm_wready),
        .m_axil_bresp_i     (dm_bresp),
        .m_axil_bvalid_i    (dm_bvalid),
        .m_axil_bready_o    (dm_bready)
    );

    // =====================================================================
    // Firewall Instantiation
    //   M0 (0x00000–0x0FFFF) → s_axi (upstream data path)
    //   M1 (0x10000–0x1FFFF) → s_mgmt (management registers)
    // =====================================================================
    loom_axil_firewall #(
        .ADDR_WIDTH           (ADDR_WIDTH),
        .TIMEOUT_CYCLES_INIT  (50),
        .MAX_OUTSTANDING_INIT (4)
    ) u_firewall (
        .clk_i          (clk),
        .rst_ni         (rst_n),

        // Upstream slave (from demux M0)
        .s_axi_awaddr   (dm_awaddr[0*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_axi_awprot   (3'b000),
        .s_axi_awvalid  (dm_awvalid[0]),
        .s_axi_awready  (dm_awready[0]),
        .s_axi_wdata    (dm_wdata[0*32 +: 32]),
        .s_axi_wstrb    (dm_wstrb[0*4 +: 4]),
        .s_axi_wvalid   (dm_wvalid[0]),
        .s_axi_wready   (dm_wready[0]),
        .s_axi_bresp    (dm_bresp[0*2 +: 2]),
        .s_axi_bvalid   (dm_bvalid[0]),
        .s_axi_bready   (dm_bready[0]),

        .s_axi_araddr   (dm_araddr[0*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_axi_arprot   (3'b000),
        .s_axi_arvalid  (dm_arvalid[0]),
        .s_axi_arready  (dm_arready[0]),
        .s_axi_rdata    (dm_rdata[0*32 +: 32]),
        .s_axi_rresp    (dm_rresp[0*2 +: 2]),
        .s_axi_rvalid   (dm_rvalid[0]),
        .s_axi_rready   (dm_rready[0]),

        // Downstream master (to test slave data port)
        .m_axi_awaddr   (fw_m_awaddr),
        .m_axi_awprot   (fw_m_awprot),
        .m_axi_awvalid  (fw_m_awvalid),
        .m_axi_awready  (fw_m_awready),
        .m_axi_wdata    (fw_m_wdata),
        .m_axi_wstrb    (fw_m_wstrb),
        .m_axi_wvalid   (fw_m_wvalid),
        .m_axi_wready   (fw_m_wready),
        .m_axi_bresp    (fw_m_bresp),
        .m_axi_bvalid   (fw_m_bvalid),
        .m_axi_bready   (fw_m_bready),

        .m_axi_araddr   (fw_m_araddr),
        .m_axi_arprot   (fw_m_arprot),
        .m_axi_arvalid  (fw_m_arvalid),
        .m_axi_arready  (fw_m_arready),
        .m_axi_rdata    (fw_m_rdata),
        .m_axi_rresp    (fw_m_rresp),
        .m_axi_rvalid   (fw_m_rvalid),
        .m_axi_rready   (fw_m_rready),

        // Management (from demux M1)
        .s_mgmt_awaddr  (dm_awaddr[1*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_mgmt_awvalid (dm_awvalid[1]),
        .s_mgmt_awready (dm_awready[1]),
        .s_mgmt_wdata   (dm_wdata[1*32 +: 32]),
        .s_mgmt_wstrb   (dm_wstrb[1*4 +: 4]),
        .s_mgmt_wvalid  (dm_wvalid[1]),
        .s_mgmt_wready  (dm_wready[1]),
        .s_mgmt_bresp   (dm_bresp[1*2 +: 2]),
        .s_mgmt_bvalid  (dm_bvalid[1]),
        .s_mgmt_bready  (dm_bready[1]),

        .s_mgmt_araddr  (dm_araddr[1*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_mgmt_arvalid (dm_arvalid[1]),
        .s_mgmt_arready (dm_arready[1]),
        .s_mgmt_rdata   (dm_rdata[1*32 +: 32]),
        .s_mgmt_rresp   (dm_rresp[1*2 +: 2]),
        .s_mgmt_rvalid  (dm_rvalid[1]),
        .s_mgmt_rready  (dm_rready[1]),

        // Sideband
        .decouple_i         (1'b0),
        .decouple_status_o  (),
        .evt_timeout_o      (),
        .evt_unsolicited_o  (),
        .irq_o              (fw_irq)
    );

    // =====================================================================
    // Test Slave Instantiation
    //   Data port ← firewall m_axi
    //   Ctrl port ← demux M2 (0x20000–0x2FFFF)
    // =====================================================================
    fw_test_slave #(
        .ADDR_WIDTH (ADDR_WIDTH)
    ) u_slave (
        .clk_i          (clk),
        .rst_ni         (rst_n),

        // Control port (from demux M2)
        .s_ctrl_awaddr  (dm_awaddr[2*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_ctrl_awvalid (dm_awvalid[2]),
        .s_ctrl_awready (dm_awready[2]),
        .s_ctrl_wdata   (dm_wdata[2*32 +: 32]),
        .s_ctrl_wstrb   (dm_wstrb[2*4 +: 4]),
        .s_ctrl_wvalid  (dm_wvalid[2]),
        .s_ctrl_wready  (dm_wready[2]),
        .s_ctrl_bresp   (dm_bresp[2*2 +: 2]),
        .s_ctrl_bvalid  (dm_bvalid[2]),
        .s_ctrl_bready  (dm_bready[2]),

        .s_ctrl_araddr  (dm_araddr[2*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_ctrl_arvalid (dm_arvalid[2]),
        .s_ctrl_arready (dm_arready[2]),
        .s_ctrl_rdata   (dm_rdata[2*32 +: 32]),
        .s_ctrl_rresp   (dm_rresp[2*2 +: 2]),
        .s_ctrl_rvalid  (dm_rvalid[2]),
        .s_ctrl_rready  (dm_rready[2]),

        // Data port (from firewall downstream)
        .s_data_awaddr  (fw_m_awaddr),
        .s_data_awvalid (fw_m_awvalid),
        .s_data_awready (fw_m_awready),
        .s_data_wdata   (fw_m_wdata),
        .s_data_wstrb   (fw_m_wstrb),
        .s_data_wvalid  (fw_m_wvalid),
        .s_data_wready  (fw_m_wready),
        .s_data_bresp   (fw_m_bresp),
        .s_data_bvalid  (fw_m_bvalid),
        .s_data_bready  (fw_m_bready),

        .s_data_araddr  (fw_m_araddr),
        .s_data_arvalid (fw_m_arvalid),
        .s_data_arready (fw_m_arready),
        .s_data_rdata   (fw_m_rdata),
        .s_data_rresp   (fw_m_rresp),
        .s_data_rvalid  (fw_m_rvalid),
        .s_data_rready  (fw_m_rready),

        .quit_o         (slave_quit)
    );

    // =====================================================================
    // SV Assertions (immediate assertions in always_ff for Verilator)
    // =====================================================================

    // A1: Lockdown blocks upstream — when lockdown is active, upstream must
    // not accept new read or write requests.
    always_ff @(posedge clk) begin
        if (rst_n && u_firewall.ctrl_lockdown_q) begin
            assert (!u_firewall.s_axi_arready)
                else $error("A1: arready asserted during lockdown");
            assert (!u_firewall.s_axi_awready)
                else $error("A1: awready asserted during lockdown");
        end
    end

    // A2: Outstanding limit — outstanding counters must never exceed the
    // configured maximum.
    always_ff @(posedge clk) begin
        if (rst_n) begin
            assert (u_firewall.wr_outstanding_q <= u_firewall.max_outstanding_q[3:0])
                else $error("A2: wr_outstanding_q (%0d) > max (%0d)",
                    u_firewall.wr_outstanding_q, u_firewall.max_outstanding_q[3:0]);
            assert (u_firewall.rd_outstanding_q <= u_firewall.max_outstanding_q[3:0])
                else $error("A2: rd_outstanding_q (%0d) > max (%0d)",
                    u_firewall.rd_outstanding_q, u_firewall.max_outstanding_q[3:0]);
        end
    end

endmodule
