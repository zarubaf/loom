// SPDX-License-Identifier: Apache-2.0
// Clock gating cell for Loom emulation
//
// This module gates the clock based on clock enable signal.
// For simulation, it uses simple AND logic.
// For synthesis on FPGA, this could be replaced with a BUFGCE or similar.

module loom_clk_gate (
    input  logic clk_i,   // Input clock
    input  logic ce_i,    // Clock enable (active high)
    output logic clk_o    // Gated clock output
);

    // Simple clock gating for simulation
    // ce_i=1: clock passes through
    // ce_i=0: clock held low (gated)
    assign clk_o = clk_i & ce_i;

endmodule
