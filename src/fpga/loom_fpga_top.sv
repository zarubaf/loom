// SPDX-License-Identifier: Apache-2.0
// Loom FPGA Top-Level for Alveo U250
//
// Instantiates PCIe XDMA, clock generator, CDC, and loom_emu_top.
// Two-level address demux:
//   XDMA AXI-Lite (1 MB @ 125 MHz)
//     ├── [0x0_0000 – 0x7_FFFF] → CDC → loom_emu_top (@ emu_clk)
//     └── [0x8_0000 – 0xF_FFFF] → xlnx_clk_gen DRP (@ 125 MHz, no CDC)

`timescale 1ns/1ps

module loom_fpga_top (
    // PCIe reference clock (100 MHz)
    input  wire        pcie_refclk_p,
    input  wire        pcie_refclk_n,
    input  wire        pcie_perst_n,
    output wire [1:0]  pci_exp_txp,
    output wire [1:0]  pci_exp_txn,
    input  wire [1:0]  pci_exp_rxp,
    input  wire [1:0]  pci_exp_rxn,

    // Emulation reference clock (300 MHz)
    input  wire        refclk_p,
    input  wire        refclk_n
);

    // =========================================================================
    // Parameters
    // =========================================================================
    localparam int ADDR_WIDTH = 20;
    localparam int N_IRQ      = 16;

    // =========================================================================
    // Clock Buffers
    // =========================================================================

    // PCIe reference clock (GT buffer)
    wire pcie_refclk;
    wire pcie_refclk_gt;

    IBUFDS_GTE4 #(
        .REFCLK_HROW_CK_SEL(2'b00)
    ) u_pcie_refclk_buf (
        .O     (pcie_refclk_gt),
        .ODIV2 (pcie_refclk),
        .CEB   (1'b0),
        .I     (pcie_refclk_p),
        .IB    (pcie_refclk_n)
    );

    // Emulation reference clock (300 MHz differential)
    wire refclk_300;

    IBUFDS u_emu_refclk_buf (
        .O  (refclk_300),
        .I  (refclk_p),
        .IB (refclk_n)
    );

    // =========================================================================
    // PCIe XDMA
    // =========================================================================

    wire        pcie_aclk;      // 125 MHz from XDMA
    wire        pcie_aresetn;

    // AXI-Lite master from XDMA (m_axil_araddr is 32-bit from IP)
    wire [31:0]           xdma_axil_araddr;
    wire                  xdma_axil_arvalid;
    wire                  xdma_axil_arready;
    wire [31:0]           xdma_axil_rdata;
    wire [1:0]            xdma_axil_rresp;
    wire                  xdma_axil_rvalid;
    wire                  xdma_axil_rready;

    wire [31:0]           xdma_axil_awaddr;
    wire                  xdma_axil_awvalid;
    wire                  xdma_axil_awready;
    wire [31:0]           xdma_axil_wdata;
    wire [3:0]            xdma_axil_wstrb;
    wire                  xdma_axil_wvalid;
    wire                  xdma_axil_wready;
    wire [1:0]            xdma_axil_bresp;
    wire                  xdma_axil_bvalid;
    wire                  xdma_axil_bready;

    xlnx_xdma u_xdma (
        .sys_clk    (pcie_refclk),
        .sys_clk_gt (pcie_refclk_gt),
        .sys_rst_n  (pcie_perst_n),

        .pci_exp_txp (pci_exp_txp),
        .pci_exp_txn (pci_exp_txn),
        .pci_exp_rxp (pci_exp_rxp),
        .pci_exp_rxn (pci_exp_rxn),

        .axi_aclk    (pcie_aclk),
        .axi_aresetn (pcie_aresetn),
        .user_lnk_up (),

        // AXI4 full master — unused, tie off inputs
        .m_axi_awready (1'b0),
        .m_axi_wready  (1'b0),
        .m_axi_bid     (4'd0),
        .m_axi_bresp   (2'b00),
        .m_axi_bvalid  (1'b0),
        .m_axi_arready (1'b0),
        .m_axi_rid     (4'd0),
        .m_axi_rdata   (128'd0),
        .m_axi_rresp   (2'b00),
        .m_axi_rlast   (1'b0),
        .m_axi_rvalid  (1'b0),
        .m_axi_awid    (),
        .m_axi_awaddr  (),
        .m_axi_awlen   (),
        .m_axi_awsize  (),
        .m_axi_awburst (),
        .m_axi_awprot  (),
        .m_axi_awvalid (),
        .m_axi_awlock  (),
        .m_axi_awcache (),
        .m_axi_wdata   (),
        .m_axi_wstrb   (),
        .m_axi_wlast   (),
        .m_axi_wvalid  (),
        .m_axi_bready  (),
        .m_axi_arid    (),
        .m_axi_araddr  (),
        .m_axi_arlen   (),
        .m_axi_arsize  (),
        .m_axi_arburst (),
        .m_axi_arprot  (),
        .m_axi_arvalid (),
        .m_axi_arlock  (),
        .m_axi_arcache (),
        .m_axi_rready  (),

        // AXI-Lite master
        .m_axil_araddr  (xdma_axil_araddr),
        .m_axil_arprot  (),
        .m_axil_arvalid (xdma_axil_arvalid),
        .m_axil_arready (xdma_axil_arready),
        .m_axil_rdata   (xdma_axil_rdata),
        .m_axil_rresp   (xdma_axil_rresp),
        .m_axil_rvalid  (xdma_axil_rvalid),
        .m_axil_rready  (xdma_axil_rready),

        .m_axil_awaddr  (xdma_axil_awaddr),
        .m_axil_awprot  (),
        .m_axil_awvalid (xdma_axil_awvalid),
        .m_axil_awready (xdma_axil_awready),
        .m_axil_wdata   (xdma_axil_wdata),
        .m_axil_wstrb   (xdma_axil_wstrb),
        .m_axil_wvalid  (xdma_axil_wvalid),
        .m_axil_wready  (xdma_axil_wready),
        .m_axil_bresp   (xdma_axil_bresp),
        .m_axil_bvalid  (xdma_axil_bvalid),
        .m_axil_bready  (xdma_axil_bready),

        // IRQ
        .usr_irq_req (1'b0),
        .usr_irq_ack (),
        .msi_enable  (),
        .msi_vector_width (),

        // Config management — unused
        .cfg_mgmt_addr           (19'd0),
        .cfg_mgmt_write          (1'b0),
        .cfg_mgmt_write_data     (32'd0),
        .cfg_mgmt_byte_enable    (4'd0),
        .cfg_mgmt_read           (1'b0),
        .cfg_mgmt_read_data      (),
        .cfg_mgmt_read_write_done()
    );

    // =========================================================================
    // Top-Level AXI-Lite Demux (2 masters)
    // =========================================================================
    // Master 0: emu_top path  [0x0_0000 – 0x7_FFFF]  MASK=0x80000 BASE=0x00000
    // Master 1: clk_gen DRP   [0x8_0000 – 0xF_FFFF]  MASK=0x80000 BASE=0x80000

    localparam int FPGA_N_MASTERS = 2;

    wire [FPGA_N_MASTERS*ADDR_WIDTH-1:0] fpga_m_araddr;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_arvalid;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_arready;
    wire [FPGA_N_MASTERS*32-1:0]         fpga_m_rdata;
    wire [FPGA_N_MASTERS*2-1:0]          fpga_m_rresp;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_rvalid;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_rready;

    wire [FPGA_N_MASTERS*ADDR_WIDTH-1:0] fpga_m_awaddr;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_awvalid;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_awready;
    wire [FPGA_N_MASTERS*32-1:0]         fpga_m_wdata;
    wire [FPGA_N_MASTERS*4-1:0]          fpga_m_wstrb;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_wvalid;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_wready;
    wire [FPGA_N_MASTERS*2-1:0]          fpga_m_bresp;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_bvalid;
    wire [FPGA_N_MASTERS-1:0]            fpga_m_bready;

    loom_axil_demux #(
        .ADDR_WIDTH (ADDR_WIDTH),
        .N_MASTERS  (FPGA_N_MASTERS),
        .BASE_ADDR  ({20'h80000, 20'h00000}),
        .ADDR_MASK  ({20'h80000, 20'h80000})
    ) u_fpga_demux (
        .clk_i  (pcie_aclk),
        .rst_ni (pcie_aresetn),

        // Use low ADDR_WIDTH bits from XDMA's 32-bit addresses
        .s_axil_araddr_i  (xdma_axil_araddr[ADDR_WIDTH-1:0]),
        .s_axil_arvalid_i (xdma_axil_arvalid),
        .s_axil_arready_o (xdma_axil_arready),
        .s_axil_rdata_o   (xdma_axil_rdata),
        .s_axil_rresp_o   (xdma_axil_rresp),
        .s_axil_rvalid_o  (xdma_axil_rvalid),
        .s_axil_rready_i  (xdma_axil_rready),

        .s_axil_awaddr_i  (xdma_axil_awaddr[ADDR_WIDTH-1:0]),
        .s_axil_awvalid_i (xdma_axil_awvalid),
        .s_axil_awready_o (xdma_axil_awready),
        .s_axil_wdata_i   (xdma_axil_wdata),
        .s_axil_wstrb_i   (xdma_axil_wstrb),
        .s_axil_wvalid_i  (xdma_axil_wvalid),
        .s_axil_wready_o  (xdma_axil_wready),
        .s_axil_bresp_o   (xdma_axil_bresp),
        .s_axil_bvalid_o  (xdma_axil_bvalid),
        .s_axil_bready_i  (xdma_axil_bready),

        .m_axil_araddr_o  (fpga_m_araddr),
        .m_axil_arvalid_o (fpga_m_arvalid),
        .m_axil_arready_i (fpga_m_arready),
        .m_axil_rdata_i   (fpga_m_rdata),
        .m_axil_rresp_i   (fpga_m_rresp),
        .m_axil_rvalid_i  (fpga_m_rvalid),
        .m_axil_rready_o  (fpga_m_rready),

        .m_axil_awaddr_o  (fpga_m_awaddr),
        .m_axil_awvalid_o (fpga_m_awvalid),
        .m_axil_awready_i (fpga_m_awready),
        .m_axil_wdata_o   (fpga_m_wdata),
        .m_axil_wstrb_o   (fpga_m_wstrb),
        .m_axil_wvalid_o  (fpga_m_wvalid),
        .m_axil_wready_i  (fpga_m_wready),
        .m_axil_bresp_i   (fpga_m_bresp),
        .m_axil_bvalid_i  (fpga_m_bvalid),
        .m_axil_bready_o  (fpga_m_bready)
    );

    // =========================================================================
    // Clock Generator (DRP reconfigurable)
    // =========================================================================

    wire emu_clk;
    wire clk_gen_locked;

    xlnx_clk_gen u_clk_gen (
        .clk_in1  (refclk_300),
        .clk_out1 (emu_clk),
        .locked   (clk_gen_locked),

        // DRP AXI interface (master 1 of fpga demux, on pcie_aclk)
        .s_axi_aclk    (pcie_aclk),
        .s_axi_aresetn (pcie_aresetn),
        .s_axi_araddr  (fpga_m_araddr[1*ADDR_WIDTH +: 11]),
        .s_axi_arvalid (fpga_m_arvalid[1]),
        .s_axi_arready (fpga_m_arready[1]),
        .s_axi_rdata   (fpga_m_rdata[1*32 +: 32]),
        .s_axi_rresp   (fpga_m_rresp[1*2 +: 2]),
        .s_axi_rvalid  (fpga_m_rvalid[1]),
        .s_axi_rready  (fpga_m_rready[1]),
        .s_axi_awaddr  (fpga_m_awaddr[1*ADDR_WIDTH +: 11]),
        .s_axi_awvalid (fpga_m_awvalid[1]),
        .s_axi_awready (fpga_m_awready[1]),
        .s_axi_wdata   (fpga_m_wdata[1*32 +: 32]),
        .s_axi_wstrb   (fpga_m_wstrb[1*4 +: 4]),
        .s_axi_wvalid  (fpga_m_wvalid[1]),
        .s_axi_wready  (fpga_m_wready[1]),
        .s_axi_bresp   (fpga_m_bresp[1*2 +: 2]),
        .s_axi_bvalid  (fpga_m_bvalid[1]),
        .s_axi_bready  (fpga_m_bready[1])
    );

    // =========================================================================
    // Reset Synchronizer
    // =========================================================================
    // Synchronize (pcie_aresetn & clk_gen_locked) into emu_clk domain

    wire emu_rst_n;

    wire rst_src = pcie_aresetn & clk_gen_locked;
    reg  rst_sync_q1, rst_sync_q2;

    always_ff @(posedge emu_clk or negedge rst_src) begin
        if (!rst_src) begin
            rst_sync_q1 <= 1'b0;
            rst_sync_q2 <= 1'b0;
        end else begin
            rst_sync_q1 <= 1'b1;
            rst_sync_q2 <= rst_sync_q1;
        end
    end

    assign emu_rst_n = rst_sync_q2;

    // =========================================================================
    // AXI-Lite Clock Domain Crossing (pcie_aclk → emu_clk)
    // =========================================================================

    wire [ADDR_WIDTH-1:0] cdc_axil_araddr;
    wire                  cdc_axil_arvalid;
    wire                  cdc_axil_arready;
    wire [31:0]           cdc_axil_rdata;
    wire [1:0]            cdc_axil_rresp;
    wire                  cdc_axil_rvalid;
    wire                  cdc_axil_rready;

    wire [ADDR_WIDTH-1:0] cdc_axil_awaddr;
    wire                  cdc_axil_awvalid;
    wire                  cdc_axil_awready;
    wire [31:0]           cdc_axil_wdata;
    wire [3:0]            cdc_axil_wstrb;
    wire                  cdc_axil_wvalid;
    wire                  cdc_axil_wready;
    wire [1:0]            cdc_axil_bresp;
    wire                  cdc_axil_bvalid;
    wire                  cdc_axil_bready;

    xlnx_axi_clock_converter u_cdc (
        // Source side (pcie_aclk domain)
        .s_axi_aclk    (pcie_aclk),
        .s_axi_aresetn (pcie_aresetn),
        .s_axi_araddr  (fpga_m_araddr[0*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_axi_arprot  (3'b000),
        .s_axi_arvalid (fpga_m_arvalid[0]),
        .s_axi_arready (fpga_m_arready[0]),
        .s_axi_rdata   (fpga_m_rdata[0*32 +: 32]),
        .s_axi_rresp   (fpga_m_rresp[0*2 +: 2]),
        .s_axi_rvalid  (fpga_m_rvalid[0]),
        .s_axi_rready  (fpga_m_rready[0]),
        .s_axi_awaddr  (fpga_m_awaddr[0*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_axi_awprot  (3'b000),
        .s_axi_awvalid (fpga_m_awvalid[0]),
        .s_axi_awready (fpga_m_awready[0]),
        .s_axi_wdata   (fpga_m_wdata[0*32 +: 32]),
        .s_axi_wstrb   (fpga_m_wstrb[0*4 +: 4]),
        .s_axi_wvalid  (fpga_m_wvalid[0]),
        .s_axi_wready  (fpga_m_wready[0]),
        .s_axi_bresp   (fpga_m_bresp[0*2 +: 2]),
        .s_axi_bvalid  (fpga_m_bvalid[0]),
        .s_axi_bready  (fpga_m_bready[0]),

        // Destination side (emu_clk domain)
        .m_axi_aclk    (emu_clk),
        .m_axi_aresetn (emu_rst_n),
        .m_axi_araddr  (cdc_axil_araddr),
        .m_axi_arprot  (),
        .m_axi_arvalid (cdc_axil_arvalid),
        .m_axi_arready (cdc_axil_arready),
        .m_axi_rdata   (cdc_axil_rdata),
        .m_axi_rresp   (cdc_axil_rresp),
        .m_axi_rvalid  (cdc_axil_rvalid),
        .m_axi_rready  (cdc_axil_rready),
        .m_axi_awaddr  (cdc_axil_awaddr),
        .m_axi_awprot  (),
        .m_axi_awvalid (cdc_axil_awvalid),
        .m_axi_awready (cdc_axil_awready),
        .m_axi_wdata   (cdc_axil_wdata),
        .m_axi_wstrb   (cdc_axil_wstrb),
        .m_axi_wvalid  (cdc_axil_wvalid),
        .m_axi_wready  (cdc_axil_wready),
        .m_axi_bresp   (cdc_axil_bresp),
        .m_axi_bvalid  (cdc_axil_bvalid),
        .m_axi_bready  (cdc_axil_bready)
    );

    // =========================================================================
    // Loom Emulation Top (UNCHANGED — same module as simulation)
    // =========================================================================

    wire [N_IRQ-1:0] irq;
    wire             finish;

    loom_emu_top u_emu_top (
        .clk_i  (emu_clk),
        .rst_ni (emu_rst_n),

        .s_axil_araddr_i  (cdc_axil_araddr),
        .s_axil_arvalid_i (cdc_axil_arvalid),
        .s_axil_arready_o (cdc_axil_arready),
        .s_axil_rdata_o   (cdc_axil_rdata),
        .s_axil_rresp_o   (cdc_axil_rresp),
        .s_axil_rvalid_o  (cdc_axil_rvalid),
        .s_axil_rready_i  (cdc_axil_rready),

        .s_axil_awaddr_i  (cdc_axil_awaddr),
        .s_axil_awvalid_i (cdc_axil_awvalid),
        .s_axil_awready_o (cdc_axil_awready),
        .s_axil_wdata_i   (cdc_axil_wdata),
        .s_axil_wstrb_i   (cdc_axil_wstrb),
        .s_axil_wvalid_i  (cdc_axil_wvalid),
        .s_axil_wready_o  (cdc_axil_wready),
        .s_axil_bresp_o   (cdc_axil_bresp),
        .s_axil_bvalid_o  (cdc_axil_bvalid),
        .s_axil_bready_i  (cdc_axil_bready),

        .irq_o    (irq),
        .finish_o (finish)
    );

endmodule
