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
    input  wire [19:0] s_intf0_awaddr,
    input  wire [2:0]  s_intf0_awprot,
    input  wire        s_intf0_awvalid,
    output wire        s_intf0_awready,
    output wire [19:0] rp_intf0_awaddr,
    output wire [2:0]  rp_intf0_awprot,
    output wire        rp_intf0_awvalid,
    input  wire        rp_intf0_awready,

    // W channel
    input  wire [31:0] s_intf0_wdata,
    input  wire [3:0]  s_intf0_wstrb,
    input  wire        s_intf0_wvalid,
    output wire        s_intf0_wready,
    output wire [31:0] rp_intf0_wdata,
    output wire [3:0]  rp_intf0_wstrb,
    output wire        rp_intf0_wvalid,
    input  wire        rp_intf0_wready,

    // B channel
    output wire [1:0]  s_intf0_bresp,
    output wire        s_intf0_bvalid,
    input  wire        s_intf0_bready,
    input  wire [1:0]  rp_intf0_bresp,
    input  wire        rp_intf0_bvalid,
    output wire        rp_intf0_bready,

    // AR channel
    input  wire [19:0] s_intf0_araddr,
    input  wire [2:0]  s_intf0_arprot,
    input  wire        s_intf0_arvalid,
    output wire        s_intf0_arready,
    output wire [19:0] rp_intf0_araddr,
    output wire [2:0]  rp_intf0_arprot,
    output wire        rp_intf0_arvalid,
    input  wire        rp_intf0_arready,

    // R channel
    output wire [31:0] s_intf0_rdata,
    output wire [1:0]  s_intf0_rresp,
    output wire        s_intf0_rvalid,
    input  wire        s_intf0_rready,
    input  wire [31:0] rp_intf0_rdata,
    input  wire [1:0]  rp_intf0_rresp,
    input  wire        rp_intf0_rvalid,
    output wire        rp_intf0_rready,

    // =====================================================================
    // Interface 1: AXI4 full (static side = s_intf1, RP side = rp_intf1)
    // =====================================================================

    // AW channel
    input  wire [3:0]  s_intf1_awid,
    input  wire [63:0] s_intf1_awaddr,
    input  wire [7:0]  s_intf1_awlen,
    input  wire [2:0]  s_intf1_awsize,
    input  wire [1:0]  s_intf1_awburst,
    input  wire        s_intf1_awlock,
    input  wire [3:0]  s_intf1_awcache,
    input  wire [2:0]  s_intf1_awprot,
    input  wire        s_intf1_awvalid,
    output wire        s_intf1_awready,
    output wire [3:0]  rp_intf1_awid,
    output wire [63:0] rp_intf1_awaddr,
    output wire [7:0]  rp_intf1_awlen,
    output wire [2:0]  rp_intf1_awsize,
    output wire [1:0]  rp_intf1_awburst,
    output wire        rp_intf1_awlock,
    output wire [3:0]  rp_intf1_awcache,
    output wire [2:0]  rp_intf1_awprot,
    output wire        rp_intf1_awvalid,
    input  wire        rp_intf1_awready,

    // W channel
    input  wire [127:0] s_intf1_wdata,
    input  wire [15:0]  s_intf1_wstrb,
    input  wire         s_intf1_wlast,
    input  wire         s_intf1_wvalid,
    output wire         s_intf1_wready,
    output wire [127:0] rp_intf1_wdata,
    output wire [15:0]  rp_intf1_wstrb,
    output wire         rp_intf1_wlast,
    output wire         rp_intf1_wvalid,
    input  wire         rp_intf1_wready,

    // B channel
    output wire [3:0]  s_intf1_bid,
    output wire [1:0]  s_intf1_bresp,
    output wire        s_intf1_bvalid,
    input  wire        s_intf1_bready,
    input  wire [3:0]  rp_intf1_bid,
    input  wire [1:0]  rp_intf1_bresp,
    input  wire        rp_intf1_bvalid,
    output wire        rp_intf1_bready,

    // AR channel
    input  wire [3:0]  s_intf1_arid,
    input  wire [63:0] s_intf1_araddr,
    input  wire [7:0]  s_intf1_arlen,
    input  wire [2:0]  s_intf1_arsize,
    input  wire [1:0]  s_intf1_arburst,
    input  wire        s_intf1_arlock,
    input  wire [3:0]  s_intf1_arcache,
    input  wire [2:0]  s_intf1_arprot,
    input  wire        s_intf1_arvalid,
    output wire        s_intf1_arready,
    output wire [3:0]  rp_intf1_arid,
    output wire [63:0] rp_intf1_araddr,
    output wire [7:0]  rp_intf1_arlen,
    output wire [2:0]  rp_intf1_arsize,
    output wire [1:0]  rp_intf1_arburst,
    output wire        rp_intf1_arlock,
    output wire [3:0]  rp_intf1_arcache,
    output wire [2:0]  rp_intf1_arprot,
    output wire        rp_intf1_arvalid,
    input  wire        rp_intf1_arready,

    // R channel
    output wire [3:0]   s_intf1_rid,
    output wire [127:0] s_intf1_rdata,
    output wire [1:0]   s_intf1_rresp,
    output wire         s_intf1_rlast,
    output wire         s_intf1_rvalid,
    input  wire         s_intf1_rready,
    input  wire [3:0]   rp_intf1_rid,
    input  wire [127:0] rp_intf1_rdata,
    input  wire [1:0]   rp_intf1_rresp,
    input  wire         rp_intf1_rlast,
    input  wire         rp_intf1_rvalid,
    output wire         rp_intf1_rready
);

    assign decouple_status = decouple;

    // =====================================================================
    // Interface 0: AXI-Lite
    // =====================================================================

    // --- Coupled: pass through. Decoupled: absorb on static, silence RP ---

    // AW channel
    assign rp_intf0_awaddr  = s_intf0_awaddr;
    assign rp_intf0_awprot  = s_intf0_awprot;
    assign rp_intf0_awvalid = decouple ? 1'b0 : s_intf0_awvalid;
    assign s_intf0_awready  = decouple ? 1'b1 : rp_intf0_awready;

    // W channel
    assign rp_intf0_wdata   = s_intf0_wdata;
    assign rp_intf0_wstrb   = s_intf0_wstrb;
    assign rp_intf0_wvalid  = decouple ? 1'b0 : s_intf0_wvalid;
    assign s_intf0_wready   = decouple ? 1'b1 : rp_intf0_wready;

    // B channel — when decoupled, generate SLVERR for absorbed writes
    // Simple approach: in coupled mode pass through; in decoupled mode
    // return SLVERR whenever a write address+data are absorbed.
    // For full correctness we use a small FSM, but for behavioral sim
    // we use combinational logic that mirrors the Xilinx IP behavior.
    assign s_intf0_bresp    = decouple ? 2'b10 : rp_intf0_bresp;  // SLVERR when decoupled
    assign s_intf0_bvalid   = decouple ? (s_intf0_awvalid & s_intf0_wvalid) : rp_intf0_bvalid;
    assign rp_intf0_bready  = decouple ? 1'b0 : s_intf0_bready;

    // AR channel
    assign rp_intf0_araddr  = s_intf0_araddr;
    assign rp_intf0_arprot  = s_intf0_arprot;
    assign rp_intf0_arvalid = decouple ? 1'b0 : s_intf0_arvalid;
    assign s_intf0_arready  = decouple ? 1'b1 : rp_intf0_arready;

    // R channel — when decoupled, return SLVERR with 0 data for absorbed reads
    assign s_intf0_rdata    = decouple ? 32'd0         : rp_intf0_rdata;
    assign s_intf0_rresp    = decouple ? 2'b10         : rp_intf0_rresp;  // SLVERR
    assign s_intf0_rvalid   = decouple ? s_intf0_arvalid : rp_intf0_rvalid;
    assign rp_intf0_rready  = decouple ? 1'b0          : s_intf0_rready;

    // =====================================================================
    // Interface 1: AXI4 full
    // =====================================================================

    // AW channel
    assign rp_intf1_awid    = s_intf1_awid;
    assign rp_intf1_awaddr  = s_intf1_awaddr;
    assign rp_intf1_awlen   = s_intf1_awlen;
    assign rp_intf1_awsize  = s_intf1_awsize;
    assign rp_intf1_awburst = s_intf1_awburst;
    assign rp_intf1_awlock  = s_intf1_awlock;
    assign rp_intf1_awcache = s_intf1_awcache;
    assign rp_intf1_awprot  = s_intf1_awprot;
    assign rp_intf1_awvalid = decouple ? 1'b0 : s_intf1_awvalid;
    assign s_intf1_awready  = decouple ? 1'b1 : rp_intf1_awready;

    // W channel
    assign rp_intf1_wdata   = s_intf1_wdata;
    assign rp_intf1_wstrb   = s_intf1_wstrb;
    assign rp_intf1_wlast   = s_intf1_wlast;
    assign rp_intf1_wvalid  = decouple ? 1'b0 : s_intf1_wvalid;
    assign s_intf1_wready   = decouple ? 1'b1 : rp_intf1_wready;

    // B channel
    assign s_intf1_bid      = decouple ? s_intf1_awid  : rp_intf1_bid;
    assign s_intf1_bresp    = decouple ? 2'b10         : rp_intf1_bresp;
    assign s_intf1_bvalid   = decouple ? (s_intf1_awvalid & s_intf1_wvalid) : rp_intf1_bvalid;
    assign rp_intf1_bready  = decouple ? 1'b0          : s_intf1_bready;

    // AR channel
    assign rp_intf1_arid    = s_intf1_arid;
    assign rp_intf1_araddr  = s_intf1_araddr;
    assign rp_intf1_arlen   = s_intf1_arlen;
    assign rp_intf1_arsize  = s_intf1_arsize;
    assign rp_intf1_arburst = s_intf1_arburst;
    assign rp_intf1_arlock  = s_intf1_arlock;
    assign rp_intf1_arcache = s_intf1_arcache;
    assign rp_intf1_arprot  = s_intf1_arprot;
    assign rp_intf1_arvalid = decouple ? 1'b0 : s_intf1_arvalid;
    assign s_intf1_arready  = decouple ? 1'b1 : rp_intf1_arready;

    // R channel
    assign s_intf1_rid      = decouple ? s_intf1_arid  : rp_intf1_rid;
    assign s_intf1_rdata    = decouple ? 128'd0        : rp_intf1_rdata;
    assign s_intf1_rresp    = decouple ? 2'b10         : rp_intf1_rresp;
    assign s_intf1_rlast    = decouple ? 1'b1          : rp_intf1_rlast;
    assign s_intf1_rvalid   = decouple ? s_intf1_arvalid : rp_intf1_rvalid;
    assign rp_intf1_rready  = decouple ? 1'b0          : s_intf1_rready;

endmodule
