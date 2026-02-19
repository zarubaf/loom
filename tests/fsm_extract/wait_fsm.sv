// SPDX-License-Identifier: Apache-2.0
// 3-state FSM with while-wait pattern
// S0: set ready, unconditional -> S1
// S1: wait for valid (while-wait), advance -> S2 with processing
// S2: wait for ack, advance -> S0
module wait_fsm (
    input bit clk,
    input bit cmd_vld,
    input bit rsp_rdy,
    output bit cmd_rdy,
    output bit rsp_vld
);

    always @(posedge clk) begin
        cmd_rdy <= 1;

        @(posedge clk);
        while (!cmd_vld)
            @(posedge clk);

        cmd_rdy <= 0;
        rsp_vld <= 1;

        @(posedge clk);
        while (!rsp_rdy)
            @(posedge clk);

        rsp_vld <= 0;
    end

endmodule
