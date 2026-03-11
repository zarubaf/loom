// SPDX-License-Identifier: Apache-2.0
// Minimal DUT for ICAP programming e2e test.
//
// A simple free-running counter.  The test exercises the shell's
// `reconfigure` command — the DUT logic itself is not the focus.

module icap_prog (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [31:0] count_o
);

    logic [31:0] counter_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            counter_q <= 32'd0;
        else
            counter_q <= counter_q + 32'd1;
    end

    assign count_o = counter_q;

endmodule
