// SPDX-License-Identifier: Apache-2.0
// Minimal DUT for scan chain dump/restore test
//
// Two registers with known reset values:
//   counter_q    : 16'hCAFE, increments each cycle
//   step_count_q :  8'd0,    increments each cycle

module scan_dump_test (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [15:0] counter_o,
    output logic [7:0]  step_count_o
);

    logic [15:0] counter_q;
    logic [7:0]  step_count_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            counter_q    <= 16'hCAFE;
            step_count_q <= 8'd0;
        end else begin
            counter_q    <= counter_q + 16'd1;
            step_count_q <= step_count_q + 8'd1;
        end
    end

    assign counter_o    = counter_q;
    assign step_count_o = step_count_q;

endmodule
