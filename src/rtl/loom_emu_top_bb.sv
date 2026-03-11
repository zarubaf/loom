// SPDX-License-Identifier: Apache-2.0
// Black-box stub of loom_emu_top for DFX static synthesis.
// Port list must exactly match loom_emu_top.

(* black_box *)
module loom_emu_top (
    input  wire         clk_i,
    input  wire         rst_ni,

    // AXI-Lite slave interface
    input  wire [19:0]  s_axil_araddr_i,
    input  wire         s_axil_arvalid_i,
    output wire         s_axil_arready_o,
    output wire [31:0]  s_axil_rdata_o,
    output wire [1:0]   s_axil_rresp_o,
    output wire         s_axil_rvalid_o,
    input  wire         s_axil_rready_i,

    input  wire [19:0]  s_axil_awaddr_i,
    input  wire         s_axil_awvalid_i,
    output wire         s_axil_awready_o,
    input  wire [31:0]  s_axil_wdata_i,
    input  wire [3:0]   s_axil_wstrb_i,
    input  wire         s_axil_wvalid_i,
    output wire         s_axil_wready_o,
    output wire [1:0]   s_axil_bresp_o,
    output wire         s_axil_bvalid_o,
    input  wire         s_axil_bready_i,

    output wire [15:0]  irq_o,
    output wire         finish_o
);
endmodule
