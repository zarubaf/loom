// SPDX-License-Identifier: Apache-2.0
// FSM extraction test derived from top.sv rw handler
// Pattern: while-wait + memory read/write + while-wait
module multisim_rw_fsm (
    input  bit       clk,
    input  bit       cmd_vld,
    input  bit       cmd_rwb,
    input  bit [7:0] cmd_address,
    input  bit [7:0] cmd_wdata,
    input  bit       rsp_rdy,
    output bit       cmd_rdy,
    output bit       rsp_vld,
    output bit [7:0] rsp_data
);
    bit [7:0] mem [256];

    always @(posedge clk) begin
        cmd_rdy <= 1;
        @(posedge clk);
        while (!cmd_vld) begin
            @(posedge clk);
        end
        cmd_rdy <= 0;

        if (cmd_rwb) begin
            rsp_data <= mem[cmd_address];
        end else begin
            mem[cmd_address] <= cmd_wdata;
            rsp_data <= 0;
        end

        rsp_vld <= 1;
        @(posedge clk);
        while (!rsp_rdy) begin
            @(posedge clk);
        end
        rsp_vld <= 0;
    end
endmodule
