// SPDX-License-Identifier: Apache-2.0
// Copyright lowRISC contributors.
// Copyright Loom contributors.
//
// loom_clk_gate - Glitch-free clock gating cell
//
// Uses Xilinx BUFGCE for synthesis, behavioral latch for simulation.
// ce_i=1 enables clock, ce_i=0 stops clock (glitch-free).

module loom_clk_gate (
  input  logic clk_i,
  input  logic ce_i,      // Clock enable: 1=run, 0=stop
  output logic clk_o
);

`ifdef SYNTHESIS
  // Xilinx BUFGCE primitive for glitch-free clock gating
  BUFGCE u_bufgce (
    .I  (clk_i),
    .CE (ce_i),
    .O  (clk_o)
  );
`else
  // Behavioral model for simulation
  // Latch captures CE on falling edge to ensure glitch-free gating
  logic ce_latch;

  // Initialize to 1 to allow clocks during reset
  initial ce_latch = 1'b1;

  always_latch begin
    if (!clk_i) begin
      ce_latch <= ce_i;
    end
  end

  assign clk_o = clk_i & ce_latch;
`endif

endmodule
