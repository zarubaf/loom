// SPDX-License-Identifier: Apache-2.0
// Behavioral stubs for Xilinx primitives used in loom_shell
//
// These provide trivial passthrough behavior for simulation.
// On FPGA, the real Xilinx primitives are used instead.

// Differential input buffer
module IBUFDS (
    output wire O,
    input  wire I,
    input  wire IB
);
    assign O = I;
endmodule

// GT differential input buffer (for PCIe reference clock)
module IBUFDS_GTE4 #(
    parameter [1:0] REFCLK_HROW_CK_SEL = 2'b00
)(
    output wire O,
    output wire ODIV2,
    input  wire CEB,
    input  wire I,
    input  wire IB
);
    assign O     = I;
    assign ODIV2 = I;
endmodule
