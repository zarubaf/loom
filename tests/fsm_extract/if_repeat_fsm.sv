// SPDX-License-Identifier: Apache-2.0
// Simplified version of the multisim_server_pull pattern:
// if-with-timing + repeat-with-timing + while-wait
module if_repeat_fsm (
    input  bit        clk,
    input  bit        enable,
    input  bit [3:0]  delay_count,
    input  bit        ack,
    output bit        active,
    output bit        request
);

    always @(posedge clk) begin
        if (enable) begin
            active <= 1;
            repeat (delay_count) begin
                @(posedge clk);
            end
            request <= 1;
            @(posedge clk);
            while (!ack)
                @(posedge clk);
            request <= 0;
            active <= 0;
        end
    end

endmodule
