// SPDX-License-Identifier: Apache-2.0
// Basic 2-state FSM: alternates a between 1 and 0
module basic_fsm (
    input bit clk,
    output bit a
);

    always @(posedge clk) begin
        a <= 1;
        @(posedge clk);
        a <= 0;
    end
endmodule
