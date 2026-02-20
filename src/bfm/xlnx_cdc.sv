// SPDX-License-Identifier: Apache-2.0
// Behavioral AXI-Lite Clock Domain Crossing (xlnx_cdc)
//
// Simulation replacement for Xilinx AXI Clock Converter IP.
// In simulation both clocks are the same, so this is a wire passthrough.

module xlnx_cdc (
    // Source side
    input  wire        s_axi_aclk,
    input  wire        s_axi_aresetn,
    input  wire [19:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output wire        s_axi_rvalid,
    input  wire        s_axi_rready,
    input  wire [19:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output wire [1:0]  s_axi_bresp,
    output wire        s_axi_bvalid,
    input  wire        s_axi_bready,

    // Destination side
    input  wire        m_axi_aclk,
    input  wire        m_axi_aresetn,
    output wire [19:0] m_axi_araddr,
    output wire [2:0]  m_axi_arprot,
    output wire        m_axi_arvalid,
    input  wire        m_axi_arready,
    input  wire [31:0] m_axi_rdata,
    input  wire [1:0]  m_axi_rresp,
    input  wire        m_axi_rvalid,
    output wire        m_axi_rready,
    output wire [19:0] m_axi_awaddr,
    output wire [2:0]  m_axi_awprot,
    output wire        m_axi_awvalid,
    input  wire        m_axi_awready,
    output wire [31:0] m_axi_wdata,
    output wire [3:0]  m_axi_wstrb,
    output wire        m_axi_wvalid,
    input  wire        m_axi_wready,
    input  wire [1:0]  m_axi_bresp,
    input  wire        m_axi_bvalid,
    output wire        m_axi_bready
);

    // Direct wire passthrough (both clocks are the same in sim)

    // Read address channel
    assign m_axi_araddr  = s_axi_araddr;
    assign m_axi_arprot  = s_axi_arprot;
    assign m_axi_arvalid = s_axi_arvalid;
    assign s_axi_arready = m_axi_arready;

    // Read data channel
    assign s_axi_rdata  = m_axi_rdata;
    assign s_axi_rresp  = m_axi_rresp;
    assign s_axi_rvalid = m_axi_rvalid;
    assign m_axi_rready = s_axi_rready;

    // Write address channel
    assign m_axi_awaddr  = s_axi_awaddr;
    assign m_axi_awprot  = s_axi_awprot;
    assign m_axi_awvalid = s_axi_awvalid;
    assign s_axi_awready = m_axi_awready;

    // Write data channel
    assign m_axi_wdata  = s_axi_wdata;
    assign m_axi_wstrb  = s_axi_wstrb;
    assign m_axi_wvalid = s_axi_wvalid;
    assign s_axi_wready = m_axi_wready;

    // Write response channel
    assign s_axi_bresp  = m_axi_bresp;
    assign s_axi_bvalid = m_axi_bvalid;
    assign m_axi_bready = s_axi_bready;

endmodule
