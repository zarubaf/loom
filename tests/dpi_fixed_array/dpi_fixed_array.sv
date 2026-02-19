// SPDX-License-Identifier: Apache-2.0
// DPI fixed-size array test — exercises input and output fixed-size array arguments
//
// This mirrors dpi_open_array but uses fixed-size unpacked arrays:
//   output bit [31:0] data[N]   instead of   output bit [31:0] data[]
//
// On the C side, fixed-size arrays map to plain pointers (uint32_t*)
// rather than svOpenArrayHandle.

localparam int N = 4;

import "DPI-C" function int dpi_fill_array(
    input string name,
    output bit [31:0] data[N],
    input int n_elements
);

import "DPI-C" function int dpi_sum_array(
    input string name,
    input bit [31:0] data[N],
    input int n_elements
);

module dpi_fixed_array (
    input logic clk_i,
    input logic rst_ni
);

    // Packed wrapper: output fixed-size array
    function automatic int dpi_fill_array_packed(
        input string name, output logic [N*32-1:0] data, input int n);
        bit [31:0] data_unpacked[N];
        int ret;
        ret = dpi_fill_array(name, data_unpacked, n);
        for (int i = 0; i < N; i++)
            data[i*32+:32] = data_unpacked[i];
        return ret;
    endfunction

    // Packed wrapper: input fixed-size array
    function automatic int dpi_sum_array_packed(
        input string name, input logic [N*32-1:0] data, input int n);
        bit [31:0] data_unpacked[N];
        for (int i = 0; i < N; i++)
            data_unpacked[i] = data[i*32+:32];
        return dpi_sum_array(name, data_unpacked, n);
    endfunction

    typedef enum logic [2:0] {
        StIdle,
        StSumInit,
        StFill,
        StSum,
        StDone
    } state_e;

    state_e state_q;
    logic [N*32-1:0] packed_arr;
    int fill_ret, sum_init_ret, sum_ret;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q <= StIdle;
            fill_ret <= 0;
            sum_init_ret <= 0;
            sum_ret <= 0;
            packed_arr <= 128'hDEAD_BEEF_CAFE_BABE_1234_5678_9ABC_DEF0;
        end else begin
            case (state_q)
                StIdle: state_q <= StSumInit;
                StSumInit: begin
                    // Sum the non-zero initial array (input-only test)
                    sum_init_ret <= dpi_sum_array_packed("init", packed_arr, N);
                    state_q <= StFill;
                end
                StFill: begin
                    fill_ret <= dpi_fill_array_packed("test", packed_arr, N);
                    state_q <= StSum;
                end
                StSum: begin
                    sum_ret <= dpi_sum_array_packed("test", packed_arr, N);
                    state_q <= StDone;
                end
                StDone: begin
                    $display("sum_init_ret=0x%0h, fill_ret=%0d, sum_ret=0x%0h",
                             sum_init_ret, fill_ret, sum_ret);
                    $finish;
                end
            endcase
        end
    end

endmodule
