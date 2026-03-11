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

// ICAP for UltraScale+ — in-system configuration access port
//
// Behavioral model for simulation:
//   AVAIL  — always ready (no pipeline stall modeled)
//   PRDONE — pulses high 16 cycles after the last CSIB=0 write, letting the
//             host's PRDONE polling path exercise a realistic wait loop
//   PRERROR — never asserted in sim
//
// The 16-cycle gap also exercises the RTL's sticky latch: PRDONE fires once,
// the loom_icap_ctrl STATUS register captures it, and the host reads it.
module ICAPE3 #(
    parameter [31:0] DEVICE_ID        = 32'h0,
    parameter        ICAP_AUTO_SWITCH  = "DISABLE",
    parameter        SIM_CFG_FILE_NAME = "NONE"
)(
    input  wire        CLK,
    input  wire        CSIB,
    input  wire        RDWRB,
    input  wire [31:0] I,
    output wire [31:0] O,
    output wire        AVAIL,
    output wire        PRDONE,
    output wire        PRERROR
);
    // Countdown: reload to 16 on every active write cycle, decrement otherwise.
    // PRDONE fires one clock after the counter reaches 1.
    // lint_off: initial values + procedural assignments are fine in a behavioral model.
    /* verilator lint_off PROCASSINIT */
    reg [4:0] ctr_q = '0;
    reg       prdone_q = '0;
    /* verilator lint_on PROCASSINIT */

    always @(posedge CLK) begin
        if (!CSIB && !RDWRB)
            ctr_q <= 5'd16;
        else if (|ctr_q)
            ctr_q <= ctr_q - 5'd1;

        prdone_q <= (ctr_q == 5'd1);
    end

    assign AVAIL   = 1'b1;
    assign PRDONE  = prdone_q;
    assign PRERROR = 1'b0;
    assign O       = '0;

    // Suppress unused-input warnings
    logic _unused;
    assign _unused = &{I, RDWRB};
endmodule
