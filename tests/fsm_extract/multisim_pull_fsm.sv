// SPDX-License-Identifier: Apache-2.0
// FSM extraction test derived from multisim_server_pull.sv
// Pattern: conditional entry + repeat(dynamic_count) + state update
module multisim_pull_fsm (
    input  bit       clk,
    input  bit       enable,
    input  bit       data_rdy,
    input  bit       pull_result_vld,
    input  bit [7:0] pull_result_data,
    output bit       data_vld,
    output bit [7:0] data
);
    localparam int DPI_DELAY_CYCLES_INACTIVE = 3;
    localparam int DPI_DELAY_CYCLES_ACTIVE   = 0;

    int dpi_delay;
    always @(posedge clk) begin
        if (enable && (!data_vld || data_rdy)) begin
            repeat (dpi_delay) begin
                data_vld <= 0;
                @(posedge clk);
            end
            data_vld <= pull_result_vld;
            data <= pull_result_data;
            dpi_delay <= pull_result_vld ? DPI_DELAY_CYCLES_ACTIVE : DPI_DELAY_CYCLES_INACTIVE;
        end
    end
endmodule
