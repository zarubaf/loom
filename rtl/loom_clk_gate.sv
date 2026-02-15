// SPDX-License-Identifier: Apache-2.0
// loom_clk_gate.sv - Glitch-free clock gating cell
//
// Uses Xilinx BUFGCE for synthesis, behavioral latch for simulation.
// CE=1 enables clock, CE=0 stops clock (glitch-free).

module loom_clk_gate (
    input  logic clk_in,
    input  logic ce,       // Clock enable: 1=run, 0=stop
    output logic clk_out
);

`ifdef SYNTHESIS
    // Xilinx BUFGCE primitive for glitch-free clock gating
    BUFGCE u_bufgce (
        .I  (clk_in),
        .CE (ce),
        .O  (clk_out)
    );
`else
    // Behavioral model for simulation
    // Latch captures CE on falling edge to ensure glitch-free gating
    logic ce_latch;

    always_latch begin
        if (!clk_in)
            ce_latch <= ce;
    end

    assign clk_out = clk_in & ce_latch;
`endif

endmodule
