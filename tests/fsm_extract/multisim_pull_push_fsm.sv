// SPDX-License-Identifier: Apache-2.0
// FSM extraction test derived from multisim_server_pull_then_push.sv
// Pattern: repeat(dynamic) + conditional + nested while-wait + push handshake
module multisim_pull_push_fsm (
    input  bit       clk,
    input  bit       enable,
    input  bit       pull_data_rdy,
    input  bit       pull_result_vld,
    input  bit [7:0] pull_result_data,
    input  bit       push_data_vld,
    input  bit [7:0] push_data,
    output bit       pull_data_vld,
    output bit [7:0] pull_data,
    output bit       push_data_rdy
);
    localparam int DPI_DELAY_CYCLES_INACTIVE = 5;
    localparam int DPI_DELAY_CYCLES_ACTIVE   = 2;

    function static int get_inactive_dpi_delay(input int current_delay);
        int next_delay;
        if (current_delay <= 0) begin
            next_delay = 1;
        end else begin
            next_delay = current_delay << 2;
        end
        return (next_delay < DPI_DELAY_CYCLES_INACTIVE) ? next_delay : DPI_DELAY_CYCLES_INACTIVE;
    endfunction

    int dpi_delay;
    always @(posedge clk) begin
        if (enable && (!pull_data_vld || pull_data_rdy)) begin
            repeat (dpi_delay) begin
                pull_data_vld <= 0;
                @(posedge clk);
            end
            pull_data_vld <= pull_result_vld;
            pull_data <= pull_result_data;
            dpi_delay <= pull_result_vld ? DPI_DELAY_CYCLES_ACTIVE : get_inactive_dpi_delay(
                dpi_delay
            );

            if (pull_result_vld) begin
                @(posedge clk);
                pull_data_vld <= 0;
                push_data_rdy <= 1;
                @(posedge clk);
                while (!push_data_vld) begin
                    @(posedge clk);
                end
                push_data_rdy <= 0;
            end
        end
    end
endmodule
