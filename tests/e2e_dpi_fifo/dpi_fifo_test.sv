// SPDX-License-Identifier: Apache-2.0
// DPI FIFO Test Module
//
// Tests read-only DPI FIFO optimization:
//   - dpi_log(cycle, value): read-only → should use FIFO path
//   - dpi_add(a, b):         read-write → should use regfile path
//   - dpi_report_result:     read-write → regfile path
//
// Ordering: every cycle calls dpi_log; every 4th cycle calls dpi_add.
// After N_ITER iterations, reports result and finishes.

import "DPI-C" function void dpi_log(input int cycle, input int value);
import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function int dpi_report_result(input int passed, input int failed);

module dpi_fifo_test (
    input  logic clk_i,
    input  logic rst_ni
);

    localparam int unsigned N_ITER = 32;

    typedef enum logic [2:0] {
        StIdle,
        StLog,
        StCallAdd,
        StCheck,
        StNext,
        StReport,
        StDone
    } state_e;

    state_e        state_q;
    logic [31:0]   result_q;
    logic [15:0]   lfsr_q;
    logic [5:0]    iter_q;
    logic [5:0]    n_pass_q;
    logic [5:0]    n_fail_q;
    logic [15:0]   arg_a_q;
    logic [15:0]   arg_b_q;

    // LFSR next value
    logic [15:0] lfsr_next;
    assign lfsr_next = {1'b0, lfsr_q[15:1]}
                     ^ (lfsr_q[0] ? 16'hB400 : 16'h0000);

    // Local reference adder
    logic [31:0] expected;
    assign expected = {16'd0, arg_a_q} + {16'd0, arg_b_q};

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q  <= StIdle;
            result_q <= 32'd0;
            lfsr_q   <= 16'hACE1;
            iter_q   <= 6'd0;
            n_pass_q <= 6'd0;
            n_fail_q <= 6'd0;
            arg_a_q  <= 16'd0;
            arg_b_q  <= 16'd0;
        end else begin
            unique case (state_q)
                StIdle: begin
                    state_q <= StLog;
                end

                StLog: begin
                    // Read-only DPI call → should go through FIFO
                    dpi_log({26'd0, iter_q}, {16'd0, lfsr_q});
                    arg_a_q <= lfsr_q;
                    lfsr_q  <= lfsr_next;
                    // Every 4th iteration, do an add
                    if (iter_q[1:0] == 2'b11) begin
                        state_q <= StCallAdd;
                    end else begin
                        state_q <= StNext;
                    end
                end

                StCallAdd: begin
                    // Read-write DPI call → must go through regfile
                    arg_b_q  <= lfsr_q;
                    lfsr_q   <= lfsr_next;
                    result_q <= dpi_add({16'd0, arg_a_q}, {16'd0, lfsr_q});
                    state_q  <= StCheck;
                end

                StCheck: begin
                    if (result_q == expected) begin
                        n_pass_q <= n_pass_q + 6'd1;
                    end else begin
                        n_fail_q <= n_fail_q + 6'd1;
                    end
                    state_q <= StNext;
                end

                StNext: begin
                    if (iter_q == N_ITER - 1) begin
                        state_q <= StReport;
                    end else begin
                        iter_q  <= iter_q + 6'd1;
                        state_q <= StLog;
                    end
                end

                StReport: begin
                    result_q <= dpi_report_result({26'd0, n_pass_q}, {26'd0, n_fail_q});
                    state_q  <= StDone;
                end

                StDone: begin
                    $finish;
                end

                default: state_q <= StIdle;
            endcase
        end
    end

endmodule
