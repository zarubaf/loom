// SPDX-License-Identifier: Apache-2.0
// Behavioral XDMA (xlnx_xdma)
//
// Simulation replacement for Xilinx XDMA IP. Wraps the socket-based BFM
// and generates clocks/resets. AXI4 full master outputs are tied to zero.
//
// Port interface matches the real xlnx_xdma instantiation in loom_shell.

module xlnx_xdma (
    input  wire        sys_clk,
    input  wire        sys_clk_gt,
    input  wire        sys_rst_n,

    output wire [1:0]  pci_exp_txp,
    output wire [1:0]  pci_exp_txn,
    input  wire [1:0]  pci_exp_rxp,
    input  wire [1:0]  pci_exp_rxn,

    output wire        axi_aclk,
    output wire        axi_aresetn,
    output wire        user_lnk_up,

    // AXI4 full master (active master — directly connected to decoupler)
    input  wire        m_axi_awready,
    input  wire        m_axi_wready,
    input  wire [3:0]  m_axi_bid,
    input  wire [1:0]  m_axi_bresp,
    input  wire        m_axi_bvalid,
    input  wire        m_axi_arready,
    input  wire [3:0]  m_axi_rid,
    input  wire [127:0] m_axi_rdata,
    input  wire [1:0]  m_axi_rresp,
    input  wire        m_axi_rlast,
    input  wire        m_axi_rvalid,
    output wire [3:0]  m_axi_awid,
    output wire [63:0] m_axi_awaddr,
    output wire [7:0]  m_axi_awlen,
    output wire [2:0]  m_axi_awsize,
    output wire [1:0]  m_axi_awburst,
    output wire [2:0]  m_axi_awprot,
    output wire        m_axi_awvalid,
    output wire        m_axi_awlock,
    output wire [3:0]  m_axi_awcache,
    output wire [127:0] m_axi_wdata,
    output wire [15:0] m_axi_wstrb,
    output wire        m_axi_wlast,
    output wire        m_axi_wvalid,
    output wire        m_axi_bready,
    output wire [3:0]  m_axi_arid,
    output wire [63:0] m_axi_araddr,
    output wire [7:0]  m_axi_arlen,
    output wire [2:0]  m_axi_arsize,
    output wire [1:0]  m_axi_arburst,
    output wire [2:0]  m_axi_arprot,
    output wire        m_axi_arvalid,
    output wire        m_axi_arlock,
    output wire [3:0]  m_axi_arcache,
    output wire        m_axi_rready,

    // AXI-Lite master
    output wire [31:0] m_axil_araddr,
    output wire [2:0]  m_axil_arprot,
    output wire        m_axil_arvalid,
    input  wire        m_axil_arready,
    input  wire [31:0] m_axil_rdata,
    input  wire [1:0]  m_axil_rresp,
    input  wire        m_axil_rvalid,
    output wire        m_axil_rready,

    output wire [31:0] m_axil_awaddr,
    output wire [2:0]  m_axil_awprot,
    output wire        m_axil_awvalid,
    input  wire        m_axil_awready,
    output wire [31:0] m_axil_wdata,
    output wire [3:0]  m_axil_wstrb,
    output wire        m_axil_wvalid,
    input  wire        m_axil_wready,
    input  wire [1:0]  m_axil_bresp,
    input  wire        m_axil_bvalid,
    output wire        m_axil_bready,

    // IRQ
    input  wire [0:0]  usr_irq_req,
    output wire [0:0]  usr_irq_ack,
    output wire        msi_enable,
    output wire [2:0]  msi_vector_width,

    // Simulation-only: IRQ lines forwarded to socket BFM
    input  wire [15:0] loom_irq_i,

    // Config management
    input  wire [18:0] cfg_mgmt_addr,
    input  wire        cfg_mgmt_write,
    input  wire [31:0] cfg_mgmt_write_data,
    input  wire [3:0]  cfg_mgmt_byte_enable,
    input  wire        cfg_mgmt_read,
    output wire [31:0] cfg_mgmt_read_data,
    output wire        cfg_mgmt_read_write_done
);

    // =========================================================================
    // Parameters
    // =========================================================================
    localparam int ADDR_WIDTH = 20;
    localparam int N_IRQ      = 16;

    // =========================================================================
    // Clock Generation (100 MHz)
    // =========================================================================
    reg clk_reg;

    initial begin
        clk_reg = 1'b0;
        forever #5 clk_reg = ~clk_reg;  // 100 MHz
    end

    assign axi_aclk = clk_reg;

    // =========================================================================
    // Reset Generation (deasserted after 100ns)
    // =========================================================================
    reg aresetn_reg;

    initial begin
        aresetn_reg = 1'b0;
        #100;
        aresetn_reg = 1'b1;
    end

    assign axi_aresetn = aresetn_reg;
    assign user_lnk_up = aresetn_reg;

    // =========================================================================
    // PCIe outputs — tied off
    // =========================================================================
    assign pci_exp_txp = 2'b0;
    assign pci_exp_txn = 2'b0;

    // =========================================================================
    // AXI4 full master — all outputs driven to zero
    // =========================================================================
    assign m_axi_awid    = 4'd0;
    assign m_axi_awaddr  = 64'd0;
    assign m_axi_awlen   = 8'd0;
    assign m_axi_awsize  = 3'd0;
    assign m_axi_awburst = 2'd0;
    assign m_axi_awprot  = 3'd0;
    assign m_axi_awvalid = 1'b0;
    assign m_axi_awlock  = 1'b0;
    assign m_axi_awcache = 4'd0;
    assign m_axi_wdata   = 128'd0;
    assign m_axi_wstrb   = 16'd0;
    assign m_axi_wlast   = 1'b0;
    assign m_axi_wvalid  = 1'b0;
    assign m_axi_bready  = 1'b0;
    assign m_axi_arid    = 4'd0;
    assign m_axi_araddr  = 64'd0;
    assign m_axi_arlen   = 8'd0;
    assign m_axi_arsize  = 3'd0;
    assign m_axi_arburst = 2'd0;
    assign m_axi_arprot  = 3'd0;
    assign m_axi_arvalid = 1'b0;
    assign m_axi_arlock  = 1'b0;
    assign m_axi_arcache = 4'd0;
    assign m_axi_rready  = 1'b0;

    // =========================================================================
    // Config management — tied off
    // =========================================================================
    assign cfg_mgmt_read_data      = 32'd0;
    assign cfg_mgmt_read_write_done = 1'b0;

    // =========================================================================
    // IRQ — acknowledge immediately
    // =========================================================================
    assign usr_irq_ack     = 1'b0;
    assign msi_enable      = 1'b0;
    assign msi_vector_width = 3'd0;

    // =========================================================================
    // AXI-Lite prot — unused
    // =========================================================================
    assign m_axil_arprot = 3'b000;
    assign m_axil_awprot = 3'b000;

    // =========================================================================
    // Socket BFM — generates AXI-Lite transactions
    // =========================================================================
    wire [ADDR_WIDTH-1:0] bfm_araddr;
    wire                  bfm_arvalid;
    wire                  bfm_arready;
    wire [31:0]           bfm_rdata;
    wire [1:0]            bfm_rresp;
    wire                  bfm_rvalid;
    wire                  bfm_rready;

    wire [ADDR_WIDTH-1:0] bfm_awaddr;
    wire                  bfm_awvalid;
    wire                  bfm_awready;
    wire [31:0]           bfm_wdata;
    wire [3:0]            bfm_wstrb;
    wire                  bfm_wvalid;
    wire                  bfm_wready;
    wire [1:0]            bfm_bresp;
    wire                  bfm_bvalid;
    wire                  bfm_bready;

    wire                  bfm_shutdown;

    loom_axil_socket_bfm #(
        .ADDR_WIDTH (ADDR_WIDTH),
        .N_IRQ      (N_IRQ)
    ) u_bfm (
        .clk_i  (axi_aclk),
        .rst_ni (axi_aresetn),

        .m_axil_araddr_o  (bfm_araddr),
        .m_axil_arvalid_o (bfm_arvalid),
        .m_axil_arready_i (bfm_arready),
        .m_axil_rdata_i   (bfm_rdata),
        .m_axil_rresp_i   (bfm_rresp),
        .m_axil_rvalid_i  (bfm_rvalid),
        .m_axil_rready_o  (bfm_rready),

        .m_axil_awaddr_o  (bfm_awaddr),
        .m_axil_awvalid_o (bfm_awvalid),
        .m_axil_awready_i (bfm_awready),
        .m_axil_wdata_o   (bfm_wdata),
        .m_axil_wstrb_o   (bfm_wstrb),
        .m_axil_wvalid_o  (bfm_wvalid),
        .m_axil_wready_i  (bfm_wready),
        .m_axil_bresp_i   (bfm_bresp),
        .m_axil_bvalid_i  (bfm_bvalid),
        .m_axil_bready_o  (bfm_bready),

        .irq_i     (loom_irq_i),
        .finish_i  (usr_irq_req[0]),
        .shutdown_o(bfm_shutdown)
    );

    // Zero-extend BFM's 20-bit addresses to 32-bit
    assign m_axil_araddr  = {12'd0, bfm_araddr};
    assign m_axil_arvalid = bfm_arvalid;
    assign bfm_arready    = m_axil_arready;
    assign bfm_rdata      = m_axil_rdata;
    assign bfm_rresp      = m_axil_rresp;
    assign bfm_rvalid     = m_axil_rvalid;
    assign m_axil_rready  = bfm_rready;

    assign m_axil_awaddr  = {12'd0, bfm_awaddr};
    assign m_axil_awvalid = bfm_awvalid;
    assign bfm_awready    = m_axil_awready;
    assign m_axil_wdata   = bfm_wdata;
    assign m_axil_wstrb   = bfm_wstrb;
    assign m_axil_wvalid  = bfm_wvalid;
    assign bfm_wready     = m_axil_wready;
    assign bfm_bresp      = m_axil_bresp;
    assign bfm_bvalid     = m_axil_bvalid;
    assign m_axil_bready  = bfm_bready;

    // =========================================================================
    // Shutdown handling
    // =========================================================================

    always @(posedge axi_aclk) begin
        if (axi_aresetn && bfm_shutdown) begin
            $display("[xdma_bfm] Shutdown complete, ending simulation");
            // Let a few cycles elapse so Verilator can flush the trace
            repeat (4) @(posedge axi_aclk);
            $finish;
        end
    end

    // =========================================================================
    // Simulation timeout (configurable via +timeout=<ns>, -1 for infinite)
    // =========================================================================
    initial begin
        longint unsigned timeout_ns;
        if (!$value$plusargs("timeout=%d", timeout_ns))
            timeout_ns = 100_000_000;  // default: 100ms
        if (timeout_ns != '1) begin
            #(timeout_ns);
            $display("[xdma_bfm] Timeout after %0d ns!", timeout_ns);
            $finish;
        end
    end

    // =========================================================================
    // Wave dump
    // =========================================================================
`ifdef VERILATOR
    initial begin
        $dumpfile("dump.fst");
        $dumpvars();
    end
`endif

endmodule
