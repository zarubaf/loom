// SPDX-License-Identifier: Apache-2.0
// Loom Shell — Unified top-level for both simulation and FPGA
//
// Instantiates all infrastructure: XDMA, AXI-Lite demux, DFX decoupler,
// CDC, clock generator, reset synchronizer, shell control register,
// and loom_emu_top.
//
// Sub-module implementations differ between sim (behavioral BFMs) and
// FPGA (Xilinx IPs), but the shell module itself is identical.
//
// Address Map (20-bit, 3 masters on aclk domain):
//   [0x0_0000 – 0x3_FFFF]  decoupler → CDC → loom_emu_top
//   [0x4_0000 – 0x4_FFFF]  xlnx_clk_gen DRP
//   [0x5_0000 – 0x5_FFFF]  shell control register

module loom_shell (
    // PCIe (XDMA)
    input  wire        pcie_refclk_p,
    input  wire        pcie_refclk_n,
    input  wire        pcie_perst_n,
    output wire [1:0]  pci_exp_txp,
    output wire [1:0]  pci_exp_txn,
    input  wire [1:0]  pci_exp_rxp,
    input  wire [1:0]  pci_exp_rxn,

    // Emulation reference clock
    input  wire        refclk_p,
    input  wire        refclk_n
);

    // =========================================================================
    // Parameters
    // =========================================================================
    localparam int ADDR_WIDTH    = 20;
    localparam int N_IRQ         = 16;
    localparam int N_MASTERS     = 3;

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

    // Emulation reference clock (differential)
    wire emu_refclk;

    IBUFDS u_emu_refclk_buf (
        .O  (emu_refclk),
        .I  (refclk_p),
        .IB (refclk_n)
    );

    // =========================================================================
    // 1. PCIe XDMA
    // =========================================================================

    wire        aclk;
    wire        aresetn;

    // AXI-Lite master from XDMA (32-bit addr)
    wire [31:0] xdma_axil_araddr;
    wire [2:0]  xdma_axil_arprot;
    wire        xdma_axil_arvalid;
    wire        xdma_axil_arready;
    wire [31:0] xdma_axil_rdata;
    wire [1:0]  xdma_axil_rresp;
    wire        xdma_axil_rvalid;
    wire        xdma_axil_rready;

    wire [31:0] xdma_axil_awaddr;
    wire [2:0]  xdma_axil_awprot;
    wire        xdma_axil_awvalid;
    wire        xdma_axil_awready;
    wire [31:0] xdma_axil_wdata;
    wire [3:0]  xdma_axil_wstrb;
    wire        xdma_axil_wvalid;
    wire        xdma_axil_wready;
    wire [1:0]  xdma_axil_bresp;
    wire        xdma_axil_bvalid;
    wire        xdma_axil_bready;

    // AXI4 full master from XDMA
    wire [3:0]   xdma_axi4_awid;
    wire [63:0]  xdma_axi4_awaddr;
    wire [7:0]   xdma_axi4_awlen;
    wire [2:0]   xdma_axi4_awsize;
    wire [1:0]   xdma_axi4_awburst;
    wire [2:0]   xdma_axi4_awprot;
    wire         xdma_axi4_awvalid;
    wire         xdma_axi4_awlock;
    wire [3:0]   xdma_axi4_awcache;
    wire [127:0] xdma_axi4_wdata;
    wire [15:0]  xdma_axi4_wstrb;
    wire         xdma_axi4_wlast;
    wire         xdma_axi4_wvalid;
    wire         xdma_axi4_bready;
    wire [3:0]   xdma_axi4_arid;
    wire [63:0]  xdma_axi4_araddr;
    wire [7:0]   xdma_axi4_arlen;
    wire [2:0]   xdma_axi4_arsize;
    wire [1:0]   xdma_axi4_arburst;
    wire [2:0]   xdma_axi4_arprot;
    wire         xdma_axi4_arvalid;
    wire         xdma_axi4_arlock;
    wire [3:0]   xdma_axi4_arcache;
    wire         xdma_axi4_rready;

    // AXI4 slave responses (from decoupler static side)
    wire         xdma_axi4_awready;
    wire         xdma_axi4_wready;
    wire [3:0]   xdma_axi4_bid;
    wire [1:0]   xdma_axi4_bresp;
    wire         xdma_axi4_bvalid;
    wire         xdma_axi4_arready;
    wire [3:0]   xdma_axi4_rid;
    wire [127:0] xdma_axi4_rdata;
    wire [1:0]   xdma_axi4_rresp;
    wire         xdma_axi4_rlast;
    wire         xdma_axi4_rvalid;

    // finish → IRQ
    wire [N_IRQ-1:0] irq;
    wire              finish;

    xlnx_xdma u_xdma (
        .sys_clk    (pcie_refclk),
        .sys_clk_gt (pcie_refclk_gt),
        .sys_rst_n  (pcie_perst_n),

        .pci_exp_txp (pci_exp_txp),
        .pci_exp_txn (pci_exp_txn),
        .pci_exp_rxp (pci_exp_rxp),
        .pci_exp_rxn (pci_exp_rxn),

        .axi_aclk    (aclk),
        .axi_aresetn (aresetn),
        .user_lnk_up (),

        // AXI4 full master — connected to decoupler
        .m_axi_awready (xdma_axi4_awready),
        .m_axi_wready  (xdma_axi4_wready),
        .m_axi_bid     (xdma_axi4_bid),
        .m_axi_bresp   (xdma_axi4_bresp),
        .m_axi_bvalid  (xdma_axi4_bvalid),
        .m_axi_arready (xdma_axi4_arready),
        .m_axi_rid     (xdma_axi4_rid),
        .m_axi_rdata   (xdma_axi4_rdata),
        .m_axi_rresp   (xdma_axi4_rresp),
        .m_axi_rlast   (xdma_axi4_rlast),
        .m_axi_rvalid  (xdma_axi4_rvalid),
        .m_axi_awid    (xdma_axi4_awid),
        .m_axi_awaddr  (xdma_axi4_awaddr),
        .m_axi_awlen   (xdma_axi4_awlen),
        .m_axi_awsize  (xdma_axi4_awsize),
        .m_axi_awburst (xdma_axi4_awburst),
        .m_axi_awprot  (xdma_axi4_awprot),
        .m_axi_awvalid (xdma_axi4_awvalid),
        .m_axi_awlock  (xdma_axi4_awlock),
        .m_axi_awcache (xdma_axi4_awcache),
        .m_axi_wdata   (xdma_axi4_wdata),
        .m_axi_wstrb   (xdma_axi4_wstrb),
        .m_axi_wlast   (xdma_axi4_wlast),
        .m_axi_wvalid  (xdma_axi4_wvalid),
        .m_axi_bready  (xdma_axi4_bready),
        .m_axi_arid    (xdma_axi4_arid),
        .m_axi_araddr  (xdma_axi4_araddr),
        .m_axi_arlen   (xdma_axi4_arlen),
        .m_axi_arsize  (xdma_axi4_arsize),
        .m_axi_arburst (xdma_axi4_arburst),
        .m_axi_arprot  (xdma_axi4_arprot),
        .m_axi_arvalid (xdma_axi4_arvalid),
        .m_axi_arlock  (xdma_axi4_arlock),
        .m_axi_arcache (xdma_axi4_arcache),
        .m_axi_rready  (xdma_axi4_rready),

        // AXI-Lite master
        .m_axil_araddr  (xdma_axil_araddr),
        .m_axil_arprot  (xdma_axil_arprot),
        .m_axil_arvalid (xdma_axil_arvalid),
        .m_axil_arready (xdma_axil_arready),
        .m_axil_rdata   (xdma_axil_rdata),
        .m_axil_rresp   (xdma_axil_rresp),
        .m_axil_rvalid  (xdma_axil_rvalid),
        .m_axil_rready  (xdma_axil_rready),

        .m_axil_awaddr  (xdma_axil_awaddr),
        .m_axil_awprot  (xdma_axil_awprot),
        .m_axil_awvalid (xdma_axil_awvalid),
        .m_axil_awready (xdma_axil_awready),
        .m_axil_wdata   (xdma_axil_wdata),
        .m_axil_wstrb   (xdma_axil_wstrb),
        .m_axil_wvalid  (xdma_axil_wvalid),
        .m_axil_wready  (xdma_axil_wready),
        .m_axil_bresp   (xdma_axil_bresp),
        .m_axil_bvalid  (xdma_axil_bvalid),
        .m_axil_bready  (xdma_axil_bready),

        // IRQ — finish on bit 0; full IRQ vector to BFM
        .usr_irq_req      (finish),
        .usr_irq_ack      (),
        .msi_enable       (),
        .msi_vector_width (),
        .loom_irq_i       (irq),

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
    // 2. AXI-Lite Demux (3 masters on aclk domain)
    // =========================================================================
    // Master 0: [0x0_0000 – 0x3_FFFF] decoupler → CDC → emu_top
    // Master 1: [0x4_0000 – 0x4_FFFF] clk_gen DRP
    // Master 2: [0x5_0000 – 0x5_FFFF] shell control register

    wire [N_MASTERS*ADDR_WIDTH-1:0] demux_m_araddr;
    wire [N_MASTERS-1:0]            demux_m_arvalid;
    wire [N_MASTERS-1:0]            demux_m_arready;
    wire [N_MASTERS*32-1:0]         demux_m_rdata;
    wire [N_MASTERS*2-1:0]          demux_m_rresp;
    wire [N_MASTERS-1:0]            demux_m_rvalid;
    wire [N_MASTERS-1:0]            demux_m_rready;

    wire [N_MASTERS*ADDR_WIDTH-1:0] demux_m_awaddr;
    wire [N_MASTERS-1:0]            demux_m_awvalid;
    wire [N_MASTERS-1:0]            demux_m_awready;
    wire [N_MASTERS*32-1:0]         demux_m_wdata;
    wire [N_MASTERS*4-1:0]          demux_m_wstrb;
    wire [N_MASTERS-1:0]            demux_m_wvalid;
    wire [N_MASTERS-1:0]            demux_m_wready;
    wire [N_MASTERS*2-1:0]          demux_m_bresp;
    wire [N_MASTERS-1:0]            demux_m_bvalid;
    wire [N_MASTERS-1:0]            demux_m_bready;

    loom_axil_demux #(
        .ADDR_WIDTH (ADDR_WIDTH),
        .N_MASTERS  (N_MASTERS),
        .BASE_ADDR  ({20'h50000, 20'h40000, 20'h00000}),
        .ADDR_MASK  ({20'hF0000, 20'hF0000, 20'hC0000})
    ) u_demux (
        .clk_i  (aclk),
        .rst_ni (aresetn),

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

        .m_axil_araddr_o  (demux_m_araddr),
        .m_axil_arvalid_o (demux_m_arvalid),
        .m_axil_arready_i (demux_m_arready),
        .m_axil_rdata_i   (demux_m_rdata),
        .m_axil_rresp_i   (demux_m_rresp),
        .m_axil_rvalid_i  (demux_m_rvalid),
        .m_axil_rready_o  (demux_m_rready),

        .m_axil_awaddr_o  (demux_m_awaddr),
        .m_axil_awvalid_o (demux_m_awvalid),
        .m_axil_awready_i (demux_m_awready),
        .m_axil_wdata_o   (demux_m_wdata),
        .m_axil_wstrb_o   (demux_m_wstrb),
        .m_axil_wvalid_o  (demux_m_wvalid),
        .m_axil_wready_i  (demux_m_wready),
        .m_axil_bresp_i   (demux_m_bresp),
        .m_axil_bvalid_i  (demux_m_bvalid),
        .m_axil_bready_o  (demux_m_bready)
    );

    // =========================================================================
    // 3. Shell Control Register (master 2)
    // =========================================================================
    // 0x00: bit 0 = decouple (drives xlnx_decoupler decouple pin)

    reg         decouple_q;
    reg         shell_rd_pending_q;
    reg  [31:0] shell_rdata_q;
    reg         shell_rvalid_q;
    reg         shell_wr_addr_q;
    reg         shell_wr_data_q;
    reg  [31:0] shell_wdata_q;
    reg         shell_bvalid_q;

    localparam int M2 = 2;  // Master index for shell ctrl

    assign demux_m_arready[M2] = 1'b1;
    assign demux_m_rdata[M2*32 +: 32]  = shell_rdata_q;
    assign demux_m_rresp[M2*2 +: 2]    = 2'b00;
    assign demux_m_rvalid[M2]           = shell_rvalid_q;

    assign demux_m_awready[M2] = 1'b1;
    assign demux_m_wready[M2]  = 1'b1;
    assign demux_m_bresp[M2*2 +: 2]    = 2'b00;
    assign demux_m_bvalid[M2]           = shell_bvalid_q;

    always_ff @(posedge aclk or negedge aresetn) begin
        if (!aresetn) begin
            decouple_q       <= 1'b0;
            shell_rd_pending_q <= 1'b0;
            shell_rdata_q    <= 32'd0;
            shell_rvalid_q   <= 1'b0;
            shell_wr_addr_q  <= 1'b0;
            shell_wr_data_q  <= 1'b0;
            shell_wdata_q    <= 32'd0;
            shell_bvalid_q   <= 1'b0;
        end else begin
            // Read path
            if (demux_m_arvalid[M2] && demux_m_arready[M2]) begin
                shell_rd_pending_q <= 1'b1;
            end

            if (shell_rd_pending_q && !shell_rvalid_q) begin
                shell_rvalid_q     <= 1'b1;
                shell_rdata_q      <= {31'd0, decouple_q};
                shell_rd_pending_q <= 1'b0;
            end

            if (shell_rvalid_q && demux_m_rready[M2]) begin
                shell_rvalid_q <= 1'b0;
            end

            // Write path
            if (demux_m_awvalid[M2] && demux_m_awready[M2]) begin
                shell_wr_addr_q <= 1'b1;
            end

            if (demux_m_wvalid[M2] && demux_m_wready[M2]) begin
                shell_wr_data_q <= 1'b1;
                shell_wdata_q   <= demux_m_wdata[M2*32 +: 32];
            end

            if (shell_wr_addr_q && shell_wr_data_q && !shell_bvalid_q) begin
                decouple_q      <= shell_wdata_q[0];
                shell_bvalid_q  <= 1'b1;
                shell_wr_addr_q <= 1'b0;
                shell_wr_data_q <= 1'b0;
            end

            if (shell_bvalid_q && demux_m_bready[M2]) begin
                shell_bvalid_q <= 1'b0;
            end
        end
    end

    // =========================================================================
    // 4. DFX Decoupler (master 0 AXI-Lite + AXI4 DMA path)
    // =========================================================================

    // AXI-Lite: decoupler RP side → CDC
    wire [ADDR_WIDTH-1:0] decoup_rp_araddr;
    wire [2:0]            decoup_rp_arprot;
    wire                  decoup_rp_arvalid;
    wire                  decoup_rp_arready;
    wire [31:0]           decoup_rp_rdata;
    wire [1:0]            decoup_rp_rresp;
    wire                  decoup_rp_rvalid;
    wire                  decoup_rp_rready;

    wire [ADDR_WIDTH-1:0] decoup_rp_awaddr;
    wire [2:0]            decoup_rp_awprot;
    wire                  decoup_rp_awvalid;
    wire                  decoup_rp_awready;
    wire [31:0]           decoup_rp_wdata;
    wire [3:0]            decoup_rp_wstrb;
    wire                  decoup_rp_wvalid;
    wire                  decoup_rp_wready;
    wire [1:0]            decoup_rp_bresp;
    wire                  decoup_rp_bvalid;
    wire                  decoup_rp_bready;

    xlnx_decoupler u_decoupler (
        .decouple        (decouple_q),
        .decouple_status (),

        // Interface 0 (AXI-Lite): static side = demux master 0
        .s_intf0_AWADDR  (demux_m_awaddr[0*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_intf0_AWPROT  (3'b000),
        .s_intf0_AWVALID (demux_m_awvalid[0]),
        .s_intf0_AWREADY (demux_m_awready[0]),
        .s_intf0_WDATA   (demux_m_wdata[0*32 +: 32]),
        .s_intf0_WSTRB   (demux_m_wstrb[0*4 +: 4]),
        .s_intf0_WVALID  (demux_m_wvalid[0]),
        .s_intf0_WREADY  (demux_m_wready[0]),
        .s_intf0_BRESP   (demux_m_bresp[0*2 +: 2]),
        .s_intf0_BVALID  (demux_m_bvalid[0]),
        .s_intf0_BREADY  (demux_m_bready[0]),
        .s_intf0_ARADDR  (demux_m_araddr[0*ADDR_WIDTH +: ADDR_WIDTH]),
        .s_intf0_ARPROT  (3'b000),
        .s_intf0_ARVALID (demux_m_arvalid[0]),
        .s_intf0_ARREADY (demux_m_arready[0]),
        .s_intf0_RDATA   (demux_m_rdata[0*32 +: 32]),
        .s_intf0_RRESP   (demux_m_rresp[0*2 +: 2]),
        .s_intf0_RVALID  (demux_m_rvalid[0]),
        .s_intf0_RREADY  (demux_m_rready[0]),

        // Interface 0 (AXI-Lite): RP side → CDC
        .rp_intf0_AWADDR  (decoup_rp_awaddr),
        .rp_intf0_AWPROT  (decoup_rp_awprot),
        .rp_intf0_AWVALID (decoup_rp_awvalid),
        .rp_intf0_AWREADY (decoup_rp_awready),
        .rp_intf0_WDATA   (decoup_rp_wdata),
        .rp_intf0_WSTRB   (decoup_rp_wstrb),
        .rp_intf0_WVALID  (decoup_rp_wvalid),
        .rp_intf0_WREADY  (decoup_rp_wready),
        .rp_intf0_BRESP   (decoup_rp_bresp),
        .rp_intf0_BVALID  (decoup_rp_bvalid),
        .rp_intf0_BREADY  (decoup_rp_bready),
        .rp_intf0_ARADDR  (decoup_rp_araddr),
        .rp_intf0_ARPROT  (decoup_rp_arprot),
        .rp_intf0_ARVALID (decoup_rp_arvalid),
        .rp_intf0_ARREADY (decoup_rp_arready),
        .rp_intf0_RDATA   (decoup_rp_rdata),
        .rp_intf0_RRESP   (decoup_rp_rresp),
        .rp_intf0_RVALID  (decoup_rp_rvalid),
        .rp_intf0_RREADY  (decoup_rp_rready),

        // Interface 1 (AXI4): static side = XDMA AXI4 master
        .s_intf1_AWID    (xdma_axi4_awid),
        .s_intf1_AWADDR  (xdma_axi4_awaddr),
        .s_intf1_AWLEN   (xdma_axi4_awlen),
        .s_intf1_AWSIZE  (xdma_axi4_awsize),
        .s_intf1_AWBURST (xdma_axi4_awburst),
        .s_intf1_AWLOCK  (xdma_axi4_awlock),
        .s_intf1_AWCACHE (xdma_axi4_awcache),
        .s_intf1_AWPROT  (xdma_axi4_awprot),
        .s_intf1_AWVALID (xdma_axi4_awvalid),
        .s_intf1_AWREADY (xdma_axi4_awready),
        .s_intf1_WDATA   (xdma_axi4_wdata),
        .s_intf1_WSTRB   (xdma_axi4_wstrb),
        .s_intf1_WLAST   (xdma_axi4_wlast),
        .s_intf1_WVALID  (xdma_axi4_wvalid),
        .s_intf1_WREADY  (xdma_axi4_wready),
        .s_intf1_BID     (xdma_axi4_bid),
        .s_intf1_BRESP   (xdma_axi4_bresp),
        .s_intf1_BVALID  (xdma_axi4_bvalid),
        .s_intf1_BREADY  (xdma_axi4_bready),
        .s_intf1_ARID    (xdma_axi4_arid),
        .s_intf1_ARADDR  (xdma_axi4_araddr),
        .s_intf1_ARLEN   (xdma_axi4_arlen),
        .s_intf1_ARSIZE  (xdma_axi4_arsize),
        .s_intf1_ARBURST (xdma_axi4_arburst),
        .s_intf1_ARLOCK  (xdma_axi4_arlock),
        .s_intf1_ARCACHE (xdma_axi4_arcache),
        .s_intf1_ARPROT  (xdma_axi4_arprot),
        .s_intf1_ARVALID (xdma_axi4_arvalid),
        .s_intf1_ARREADY (xdma_axi4_arready),
        .s_intf1_RID     (xdma_axi4_rid),
        .s_intf1_RDATA   (xdma_axi4_rdata),
        .s_intf1_RRESP   (xdma_axi4_rresp),
        .s_intf1_RLAST   (xdma_axi4_rlast),
        .s_intf1_RVALID  (xdma_axi4_rvalid),
        .s_intf1_RREADY  (xdma_axi4_rready),

        // Interface 1 (AXI4): RP side → error slave (future: DMA slave)
        .rp_intf1_AWID    (axi4_err_awid),
        .rp_intf1_AWADDR  (axi4_err_awaddr),
        .rp_intf1_AWLEN   (axi4_err_awlen),
        .rp_intf1_AWSIZE  (axi4_err_awsize),
        .rp_intf1_AWBURST (axi4_err_awburst),
        .rp_intf1_AWLOCK  (axi4_err_awlock),
        .rp_intf1_AWCACHE (axi4_err_awcache),
        .rp_intf1_AWPROT  (axi4_err_awprot),
        .rp_intf1_AWVALID (axi4_err_awvalid),
        .rp_intf1_AWREADY (axi4_err_awready),
        .rp_intf1_WDATA   (axi4_err_wdata),
        .rp_intf1_WSTRB   (axi4_err_wstrb),
        .rp_intf1_WLAST   (axi4_err_wlast),
        .rp_intf1_WVALID  (axi4_err_wvalid),
        .rp_intf1_WREADY  (axi4_err_wready),
        .rp_intf1_BID     (axi4_err_bid),
        .rp_intf1_BRESP   (axi4_err_bresp),
        .rp_intf1_BVALID  (axi4_err_bvalid),
        .rp_intf1_BREADY  (axi4_err_bready),
        .rp_intf1_ARID    (axi4_err_arid),
        .rp_intf1_ARADDR  (axi4_err_araddr),
        .rp_intf1_ARLEN   (axi4_err_arlen),
        .rp_intf1_ARSIZE  (axi4_err_arsize),
        .rp_intf1_ARBURST (axi4_err_arburst),
        .rp_intf1_ARLOCK  (axi4_err_arlock),
        .rp_intf1_ARCACHE (axi4_err_arcache),
        .rp_intf1_ARPROT  (axi4_err_arprot),
        .rp_intf1_ARVALID (axi4_err_arvalid),
        .rp_intf1_ARREADY (axi4_err_arready),
        .rp_intf1_RID     (axi4_err_rid),
        .rp_intf1_RDATA   (axi4_err_rdata),
        .rp_intf1_RRESP   (axi4_err_rresp),
        .rp_intf1_RLAST   (axi4_err_rlast),
        .rp_intf1_RVALID  (axi4_err_rvalid),
        .rp_intf1_RREADY  (axi4_err_rready)
    );

    // =========================================================================
    // 4b. AXI4 Error Slave (DMA RP side)
    // =========================================================================
    // The XDMA AXI4 DMA port is not used yet.  A proper error slave
    // ensures that any stray DMA transaction gets a DECERR response
    // instead of hanging the bus.

    wire [3:0]   axi4_err_awid;
    wire [63:0]  axi4_err_awaddr;
    wire [7:0]   axi4_err_awlen;
    wire [2:0]   axi4_err_awsize;
    wire [1:0]   axi4_err_awburst;
    wire         axi4_err_awlock;
    wire [3:0]   axi4_err_awcache;
    wire [2:0]   axi4_err_awprot;
    wire         axi4_err_awvalid;
    wire         axi4_err_awready;
    wire [127:0] axi4_err_wdata;
    wire [15:0]  axi4_err_wstrb;
    wire         axi4_err_wlast;
    wire         axi4_err_wvalid;
    wire         axi4_err_wready;
    wire [3:0]   axi4_err_bid;
    wire [1:0]   axi4_err_bresp;
    wire         axi4_err_bvalid;
    wire         axi4_err_bready;
    wire [3:0]   axi4_err_arid;
    wire [63:0]  axi4_err_araddr;
    wire [7:0]   axi4_err_arlen;
    wire [2:0]   axi4_err_arsize;
    wire [1:0]   axi4_err_arburst;
    wire         axi4_err_arlock;
    wire [3:0]   axi4_err_arcache;
    wire [2:0]   axi4_err_arprot;
    wire         axi4_err_arvalid;
    wire         axi4_err_arready;
    wire [3:0]   axi4_err_rid;
    wire [127:0] axi4_err_rdata;
    wire [1:0]   axi4_err_rresp;
    wire         axi4_err_rlast;
    wire         axi4_err_rvalid;
    wire         axi4_err_rready;

    loom_axi4_err_slv #(
        .ID_WIDTH   (4),
        .DATA_WIDTH (128),
        .RESP       (2'b11)  // DECERR
    ) u_axi4_err_slv (
        .clk_i  (aclk),
        .rst_ni (aresetn),

        .s_axi_awid    (axi4_err_awid),
        .s_axi_awaddr  (axi4_err_awaddr),
        .s_axi_awlen   (axi4_err_awlen),
        .s_axi_awsize  (axi4_err_awsize),
        .s_axi_awburst (axi4_err_awburst),
        .s_axi_awlock  (axi4_err_awlock),
        .s_axi_awcache (axi4_err_awcache),
        .s_axi_awprot  (axi4_err_awprot),
        .s_axi_awvalid (axi4_err_awvalid),
        .s_axi_awready (axi4_err_awready),
        .s_axi_wdata   (axi4_err_wdata),
        .s_axi_wstrb   (axi4_err_wstrb),
        .s_axi_wlast   (axi4_err_wlast),
        .s_axi_wvalid  (axi4_err_wvalid),
        .s_axi_wready  (axi4_err_wready),
        .s_axi_bid     (axi4_err_bid),
        .s_axi_bresp   (axi4_err_bresp),
        .s_axi_bvalid  (axi4_err_bvalid),
        .s_axi_bready  (axi4_err_bready),
        .s_axi_arid    (axi4_err_arid),
        .s_axi_araddr  (axi4_err_araddr),
        .s_axi_arlen   (axi4_err_arlen),
        .s_axi_arsize  (axi4_err_arsize),
        .s_axi_arburst (axi4_err_arburst),
        .s_axi_arlock  (axi4_err_arlock),
        .s_axi_arcache (axi4_err_arcache),
        .s_axi_arprot  (axi4_err_arprot),
        .s_axi_arvalid (axi4_err_arvalid),
        .s_axi_arready (axi4_err_arready),
        .s_axi_rid     (axi4_err_rid),
        .s_axi_rdata   (axi4_err_rdata),
        .s_axi_rresp   (axi4_err_rresp),
        .s_axi_rlast   (axi4_err_rlast),
        .s_axi_rvalid  (axi4_err_rvalid),
        .s_axi_rready  (axi4_err_rready)
    );

    // =========================================================================
    // 5. AXI-Lite CDC (aclk → emu_clk)
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

    wire emu_clk;
    wire emu_rst_n;

    xlnx_cdc u_cdc (
        // Source side (aclk domain — from decoupler RP)
        .s_axi_aclk    (aclk),
        .s_axi_aresetn (aresetn),
        .s_axi_araddr  (decoup_rp_araddr),
        .s_axi_arprot  (decoup_rp_arprot),
        .s_axi_arvalid (decoup_rp_arvalid),
        .s_axi_arready (decoup_rp_arready),
        .s_axi_rdata   (decoup_rp_rdata),
        .s_axi_rresp   (decoup_rp_rresp),
        .s_axi_rvalid  (decoup_rp_rvalid),
        .s_axi_rready  (decoup_rp_rready),
        .s_axi_awaddr  (decoup_rp_awaddr),
        .s_axi_awprot  (decoup_rp_awprot),
        .s_axi_awvalid (decoup_rp_awvalid),
        .s_axi_awready (decoup_rp_awready),
        .s_axi_wdata   (decoup_rp_wdata),
        .s_axi_wstrb   (decoup_rp_wstrb),
        .s_axi_wvalid  (decoup_rp_wvalid),
        .s_axi_wready  (decoup_rp_wready),
        .s_axi_bresp   (decoup_rp_bresp),
        .s_axi_bvalid  (decoup_rp_bvalid),
        .s_axi_bready  (decoup_rp_bready),

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
    // 6. Clock Generator
    // =========================================================================

    wire clk_gen_locked;

    xlnx_clk_gen u_clk_gen (
        .clk_in1  (emu_refclk),
        .clk_out1 (emu_clk),
        .locked   (clk_gen_locked),

        // DRP AXI interface (master 1 of demux, on aclk)
        .s_axi_aclk    (aclk),
        .s_axi_aresetn (aresetn),
        .s_axi_araddr  (demux_m_araddr[1*ADDR_WIDTH +: 11]),
        .s_axi_arvalid (demux_m_arvalid[1]),
        .s_axi_arready (demux_m_arready[1]),
        .s_axi_rdata   (demux_m_rdata[1*32 +: 32]),
        .s_axi_rresp   (demux_m_rresp[1*2 +: 2]),
        .s_axi_rvalid  (demux_m_rvalid[1]),
        .s_axi_rready  (demux_m_rready[1]),
        .s_axi_awaddr  (demux_m_awaddr[1*ADDR_WIDTH +: 11]),
        .s_axi_awvalid (demux_m_awvalid[1]),
        .s_axi_awready (demux_m_awready[1]),
        .s_axi_wdata   (demux_m_wdata[1*32 +: 32]),
        .s_axi_wstrb   (demux_m_wstrb[1*4 +: 4]),
        .s_axi_wvalid  (demux_m_wvalid[1]),
        .s_axi_wready  (demux_m_wready[1]),
        .s_axi_bresp   (demux_m_bresp[1*2 +: 2]),
        .s_axi_bvalid  (demux_m_bvalid[1]),
        .s_axi_bready  (demux_m_bready[1])
    );

    // =========================================================================
    // 7. Reset Synchronizer (aclk domain → emu_clk domain)
    // =========================================================================

    wire rst_src = aresetn & clk_gen_locked;
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
    // 8. Loom Emulation Top (emu_clk domain)
    // =========================================================================

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
