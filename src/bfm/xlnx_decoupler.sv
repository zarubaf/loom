// SPDX-License-Identifier: Apache-2.0
// Behavioral DFX Decoupler (xlnx_decoupler)
//
// Simulation replacement for Xilinx DFX Decoupler IP configured with:
//   Interface 0: AXI-Lite (20-bit addr, 32-bit data)
//   Interface 1: AXI4 full (64-bit addr, 128-bit data, 4-bit ID)
//
// Coupled  (decouple=0): wire passthrough on both interfaces
// Decoupled (decouple=1): static side absorbs requests (SLVERR),
//                          RP side sees no activity

module xlnx_decoupler (
    input  wire        decouple,
    output wire        decouple_status,

    // =====================================================================
    // Interface 0: AXI-Lite (static side = s_intf0, RP side = rp_intf0)
    // =====================================================================

    // AW channel
    input  wire [19:0] s_intf0_AWADDR,
    input  wire [2:0]  s_intf0_AWPROT,
    input  wire        s_intf0_AWVALID,
    output wire        s_intf0_AWREADY,
    output wire [19:0] rp_intf0_AWADDR,
    output wire [2:0]  rp_intf0_AWPROT,
    output wire        rp_intf0_AWVALID,
    input  wire        rp_intf0_AWREADY,

    // W channel
    input  wire [31:0] s_intf0_WDATA,
    input  wire [3:0]  s_intf0_WSTRB,
    input  wire        s_intf0_WVALID,
    output wire        s_intf0_WREADY,
    output wire [31:0] rp_intf0_WDATA,
    output wire [3:0]  rp_intf0_WSTRB,
    output wire        rp_intf0_WVALID,
    input  wire        rp_intf0_WREADY,

    // B channel
    output wire [1:0]  s_intf0_BRESP,
    output wire        s_intf0_BVALID,
    input  wire        s_intf0_BREADY,
    input  wire [1:0]  rp_intf0_BRESP,
    input  wire        rp_intf0_BVALID,
    output wire        rp_intf0_BREADY,

    // AR channel
    input  wire [19:0] s_intf0_ARADDR,
    input  wire [2:0]  s_intf0_ARPROT,
    input  wire        s_intf0_ARVALID,
    output wire        s_intf0_ARREADY,
    output wire [19:0] rp_intf0_ARADDR,
    output wire [2:0]  rp_intf0_ARPROT,
    output wire        rp_intf0_ARVALID,
    input  wire        rp_intf0_ARREADY,

    // R channel
    output wire [31:0] s_intf0_RDATA,
    output wire [1:0]  s_intf0_RRESP,
    output wire        s_intf0_RVALID,
    input  wire        s_intf0_RREADY,
    input  wire [31:0] rp_intf0_RDATA,
    input  wire [1:0]  rp_intf0_RRESP,
    input  wire        rp_intf0_RVALID,
    output wire        rp_intf0_RREADY,

    // =====================================================================
    // Interface 1: AXI4 full (static side = s_intf1, RP side = rp_intf1)
    // =====================================================================

    // AW channel
    input  wire [3:0]  s_intf1_AWID,
    input  wire [63:0] s_intf1_AWADDR,
    input  wire [7:0]  s_intf1_AWLEN,
    input  wire [2:0]  s_intf1_AWSIZE,
    input  wire [1:0]  s_intf1_AWBURST,
    input  wire [0:0]  s_intf1_AWLOCK,
    input  wire [3:0]  s_intf1_AWCACHE,
    input  wire [2:0]  s_intf1_AWPROT,
    input  wire        s_intf1_AWVALID,
    output wire        s_intf1_AWREADY,
    output wire [3:0]  rp_intf1_AWID,
    output wire [63:0] rp_intf1_AWADDR,
    output wire [7:0]  rp_intf1_AWLEN,
    output wire [2:0]  rp_intf1_AWSIZE,
    output wire [1:0]  rp_intf1_AWBURST,
    output wire [0:0]  rp_intf1_AWLOCK,
    output wire [3:0]  rp_intf1_AWCACHE,
    output wire [2:0]  rp_intf1_AWPROT,
    output wire        rp_intf1_AWVALID,
    input  wire        rp_intf1_AWREADY,

    // W channel
    input  wire [127:0] s_intf1_WDATA,
    input  wire [15:0]  s_intf1_WSTRB,
    input  wire         s_intf1_WLAST,
    input  wire         s_intf1_WVALID,
    output wire         s_intf1_WREADY,
    output wire [127:0] rp_intf1_WDATA,
    output wire [15:0]  rp_intf1_WSTRB,
    output wire         rp_intf1_WLAST,
    output wire         rp_intf1_WVALID,
    input  wire         rp_intf1_WREADY,

    // B channel
    output wire [3:0]  s_intf1_BID,
    output wire [1:0]  s_intf1_BRESP,
    output wire        s_intf1_BVALID,
    input  wire        s_intf1_BREADY,
    input  wire [3:0]  rp_intf1_BID,
    input  wire [1:0]  rp_intf1_BRESP,
    input  wire        rp_intf1_BVALID,
    output wire        rp_intf1_BREADY,

    // AR channel
    input  wire [3:0]  s_intf1_ARID,
    input  wire [63:0] s_intf1_ARADDR,
    input  wire [7:0]  s_intf1_ARLEN,
    input  wire [2:0]  s_intf1_ARSIZE,
    input  wire [1:0]  s_intf1_ARBURST,
    input  wire [0:0]  s_intf1_ARLOCK,
    input  wire [3:0]  s_intf1_ARCACHE,
    input  wire [2:0]  s_intf1_ARPROT,
    input  wire        s_intf1_ARVALID,
    output wire        s_intf1_ARREADY,
    output wire [3:0]  rp_intf1_ARID,
    output wire [63:0] rp_intf1_ARADDR,
    output wire [7:0]  rp_intf1_ARLEN,
    output wire [2:0]  rp_intf1_ARSIZE,
    output wire [1:0]  rp_intf1_ARBURST,
    output wire [0:0]  rp_intf1_ARLOCK,
    output wire [3:0]  rp_intf1_ARCACHE,
    output wire [2:0]  rp_intf1_ARPROT,
    output wire        rp_intf1_ARVALID,
    input  wire        rp_intf1_ARREADY,

    // R channel
    output wire [3:0]   s_intf1_RID,
    output wire [127:0] s_intf1_RDATA,
    output wire [1:0]   s_intf1_RRESP,
    output wire         s_intf1_RLAST,
    output wire         s_intf1_RVALID,
    input  wire         s_intf1_RREADY,
    input  wire [3:0]   rp_intf1_RID,
    input  wire [127:0] rp_intf1_RDATA,
    input  wire [1:0]   rp_intf1_RRESP,
    input  wire         rp_intf1_RLAST,
    input  wire         rp_intf1_RVALID,
    output wire         rp_intf1_RREADY
);

    assign decouple_status = decouple;

    // =====================================================================
    // Interface 0: AXI-Lite
    // =====================================================================

    // --- Coupled: pass through. Decoupled: absorb on static, silence RP ---

    // AW channel
    assign rp_intf0_AWADDR  = s_intf0_AWADDR;
    assign rp_intf0_AWPROT  = s_intf0_AWPROT;
    assign rp_intf0_AWVALID = decouple ? 1'b0 : s_intf0_AWVALID;
    assign s_intf0_AWREADY  = decouple ? 1'b1 : rp_intf0_AWREADY;

    // W channel
    assign rp_intf0_WDATA   = s_intf0_WDATA;
    assign rp_intf0_WSTRB   = s_intf0_WSTRB;
    assign rp_intf0_WVALID  = decouple ? 1'b0 : s_intf0_WVALID;
    assign s_intf0_WREADY   = decouple ? 1'b1 : rp_intf0_WREADY;

    // B channel — when decoupled, generate SLVERR for absorbed writes
    // Simple approach: in coupled mode pass through; in decoupled mode
    // return SLVERR whenever a write address+data are absorbed.
    // For full correctness we use a small FSM, but for behavioral sim
    // we use combinational logic that mirrors the Xilinx IP behavior.
    assign s_intf0_BRESP    = decouple ? 2'b10 : rp_intf0_BRESP;  // SLVERR when decoupled
    assign s_intf0_BVALID   = decouple ? (s_intf0_AWVALID & s_intf0_WVALID) : rp_intf0_BVALID;
    assign rp_intf0_BREADY  = decouple ? 1'b0 : s_intf0_BREADY;

    // AR channel
    assign rp_intf0_ARADDR  = s_intf0_ARADDR;
    assign rp_intf0_ARPROT  = s_intf0_ARPROT;
    assign rp_intf0_ARVALID = decouple ? 1'b0 : s_intf0_ARVALID;
    assign s_intf0_ARREADY  = decouple ? 1'b1 : rp_intf0_ARREADY;

    // R channel — when decoupled, return SLVERR with 0 data for absorbed reads
    assign s_intf0_RDATA    = decouple ? 32'd0         : rp_intf0_RDATA;
    assign s_intf0_RRESP    = decouple ? 2'b10         : rp_intf0_RRESP;  // SLVERR
    assign s_intf0_RVALID   = decouple ? s_intf0_ARVALID : rp_intf0_RVALID;
    assign rp_intf0_RREADY  = decouple ? 1'b0          : s_intf0_RREADY;

    // =====================================================================
    // Interface 1: AXI4 full
    // =====================================================================

    // AW channel
    assign rp_intf1_AWID    = s_intf1_AWID;
    assign rp_intf1_AWADDR  = s_intf1_AWADDR;
    assign rp_intf1_AWLEN   = s_intf1_AWLEN;
    assign rp_intf1_AWSIZE  = s_intf1_AWSIZE;
    assign rp_intf1_AWBURST = s_intf1_AWBURST;
    assign rp_intf1_AWLOCK  = s_intf1_AWLOCK;
    assign rp_intf1_AWCACHE = s_intf1_AWCACHE;
    assign rp_intf1_AWPROT  = s_intf1_AWPROT;
    assign rp_intf1_AWVALID = decouple ? 1'b0 : s_intf1_AWVALID;
    assign s_intf1_AWREADY  = decouple ? 1'b1 : rp_intf1_AWREADY;

    // W channel
    assign rp_intf1_WDATA   = s_intf1_WDATA;
    assign rp_intf1_WSTRB   = s_intf1_WSTRB;
    assign rp_intf1_WLAST   = s_intf1_WLAST;
    assign rp_intf1_WVALID  = decouple ? 1'b0 : s_intf1_WVALID;
    assign s_intf1_WREADY   = decouple ? 1'b1 : rp_intf1_WREADY;

    // B channel
    assign s_intf1_BID      = decouple ? s_intf1_AWID  : rp_intf1_BID;
    assign s_intf1_BRESP    = decouple ? 2'b10         : rp_intf1_BRESP;
    assign s_intf1_BVALID   = decouple ? (s_intf1_AWVALID & s_intf1_WVALID) : rp_intf1_BVALID;
    assign rp_intf1_BREADY  = decouple ? 1'b0          : s_intf1_BREADY;

    // AR channel
    assign rp_intf1_ARID    = s_intf1_ARID;
    assign rp_intf1_ARADDR  = s_intf1_ARADDR;
    assign rp_intf1_ARLEN   = s_intf1_ARLEN;
    assign rp_intf1_ARSIZE  = s_intf1_ARSIZE;
    assign rp_intf1_ARBURST = s_intf1_ARBURST;
    assign rp_intf1_ARLOCK  = s_intf1_ARLOCK;
    assign rp_intf1_ARCACHE = s_intf1_ARCACHE;
    assign rp_intf1_ARPROT  = s_intf1_ARPROT;
    assign rp_intf1_ARVALID = decouple ? 1'b0 : s_intf1_ARVALID;
    assign s_intf1_ARREADY  = decouple ? 1'b1 : rp_intf1_ARREADY;

    // R channel
    assign s_intf1_RID      = decouple ? s_intf1_ARID  : rp_intf1_RID;
    assign s_intf1_RDATA    = decouple ? 128'd0        : rp_intf1_RDATA;
    assign s_intf1_RRESP    = decouple ? 2'b10         : rp_intf1_RRESP;
    assign s_intf1_RLAST    = decouple ? 1'b1          : rp_intf1_RLAST;
    assign s_intf1_RVALID   = decouple ? s_intf1_ARVALID : rp_intf1_RVALID;
    assign rp_intf1_RREADY  = decouple ? 1'b0          : s_intf1_RREADY;

endmodule
