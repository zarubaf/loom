// SPDX-License-Identifier: Apache-2.0
// DUT for scan chain dump/restore test with DPI calls
//
// State machine sequences through 4 DPI call types, then enters
// counting mode.  All DPI return values are stored in DUT flip-flops
// and captured by the scan chain.
//
// States: StIdle -> StCallAdd -> StCallNotify -> StCallFill -> StCallSum -> StDone

localparam int N = 4;

import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function void dpi_notify(input int value);

import "DPI-C" function int dpi_fill_fixed(
    input string name,
    output bit [31:0] data[N],
    input int n
);

import "DPI-C" function int dpi_sum_open(
    input string name,
    input bit [31:0] data[],
    input int n
);

module scan_dump_test (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [15:0] counter_o,
    output logic [7:0]  step_count_o
);

    // -- Array wrappers (packed <-> unpacked) --

    // Fixed-size output array wrapper
    function automatic int dpi_fill_fixed_packed(
        input string name, output logic [N*32-1:0] data, input int n);
        bit [31:0] data_unpacked[N];
        int ret;
        ret = dpi_fill_fixed(name, data_unpacked, n);
        for (int i = 0; i < N; i++)
            data[i*32+:32] = data_unpacked[i];
        return ret;
    endfunction

    // Open-array input wrapper
    function automatic int dpi_sum_open_packed(
        input string name, input logic [N*32-1:0] data, input int n);
        bit [31:0] data_unpacked[N];
        for (int i = 0; i < N; i++)
            data_unpacked[i] = data[i*32+:32];
        return dpi_sum_open(name, data_unpacked, n);
    endfunction

    // -- State machine --

    typedef enum logic [2:0] {
        StIdle,
        StCallAdd,
        StCallNotify,
        StCallFill,
        StCallSum,
        StDone
    } state_e;

    state_e           state_q;
    logic [15:0]      counter_q;
    logic [7:0]       step_count_q;
    logic [31:0]      add_result_q;
    logic [7:0]       notify_done_q;
    logic [31:0]      fill_ret_q;
    logic [31:0]      sum_result_q;
    logic [N*32-1:0]  packed_arr;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q       <= StIdle;
            counter_q     <= 16'hCAFE;
            step_count_q  <= 8'd0;
            add_result_q  <= 32'd0;
            notify_done_q <= 8'd0;
            fill_ret_q    <= 32'd0;
            sum_result_q  <= 32'd0;
            packed_arr    <= '0;
        end else begin
            step_count_q <= step_count_q + 8'd1;

            case (state_q)
                StIdle: begin
                    state_q <= StCallAdd;
                end

                StCallAdd: begin
                    add_result_q <= dpi_add({16'd0, counter_q}, 32'h1000);
                    state_q <= StCallNotify;
                end

                StCallNotify: begin
                    dpi_notify(32'd42);
                    notify_done_q <= 8'd1;
                    state_q <= StCallFill;
                end

                StCallFill: begin
                    fill_ret_q <= dpi_fill_fixed_packed("test", packed_arr, N);
                    state_q <= StCallSum;
                end

                StCallSum: begin
                    sum_result_q <= dpi_sum_open_packed("test", packed_arr, N);
                    state_q <= StDone;
                end

                StDone: begin
                    $display("counter=%0h add=%0h notify=%0d fill=%0d sum=%0h",
                             counter_q, add_result_q, notify_done_q,
                             fill_ret_q, sum_result_q);
                    counter_q <= counter_q + 16'd1;
                end
            endcase
        end
    end

    assign counter_o    = counter_q;
    assign step_count_o = step_count_q;

endmodule
