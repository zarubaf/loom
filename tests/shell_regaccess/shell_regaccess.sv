// SPDX-License-Identifier: Apache-2.0
// Minimal DUT for shell register access test
//
// A simple counter that increments each cycle. No $finish — the host
// must explicitly exit. This exercises the shell read/write/status
// commands against a live (non-terminating) design.

module shell_regaccess (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [31:0] count_o
);

    logic [31:0] counter_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            counter_q <= 32'd0;
        end else begin
            counter_q <= counter_q + 32'd1;
        end
    end

    assign count_o = counter_q;

endmodule
