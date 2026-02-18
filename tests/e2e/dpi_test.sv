// SPDX-License-Identifier: Apache-2.0
// DPI Test Module
//
// Uses an LFSR to generate pseudo-random operands, calls dpi_add N_ITER times,
// and verifies each result against a local adder.  After all iterations the
// pass/fail count is reported via dpi_report_result and the design finishes.
//
// IMPORTANT: DPI calls must be in clocked (always_ff) blocks only.

import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function int dpi_report_result(input int passed, input int result);
import "DPI-C" function void multisim_server_start(string server_name);

module dpi_test (
    input  logic clk_i,
    input  logic rst_ni
);

    localparam int unsigned N_ITER = 8;

    // =========================================================================
    // State machine
    // =========================================================================

    typedef enum logic [2:0] {
        StIdle,
        StServerStart,
        StCallAdd,
        StCheck,
        StNext,
        StReport,
        StDone
    } state_e;

    state_e        state_q;
    logic [31:0]   result_q;
    logic [15:0]   lfsr_q;        // 16-bit Galois LFSR (taps: 16,14,13,11)
    logic [3:0]    iter_q;        // iteration counter
    logic [3:0]    n_pass_q;      // pass count
    logic [3:0]    n_fail_q;      // fail count
    logic [15:0]   arg_a_q;       // captured operand A
    logic [15:0]   arg_b_q;       // captured operand B

    // LFSR next value (Galois, maximal-length for 16 bits)
    logic [15:0] lfsr_next;
    assign lfsr_next = {1'b0, lfsr_q[15:1]}
                     ^ (lfsr_q[0] ? 16'hB400 : 16'h0000);

    // Local reference adder
    logic [31:0] expected;
    assign expected = {16'd0, arg_a_q} + {16'd0, arg_b_q};

    // =========================================================================
    // Main sequential block
    // =========================================================================

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q  <= StIdle;
            result_q <= 32'd0;
            lfsr_q   <= 16'hACE1;  // non-zero seed
            iter_q   <= 4'd0;
            n_pass_q <= 4'd0;
            n_fail_q <= 4'd0;
            arg_a_q  <= 16'd0;
            arg_b_q  <= 16'd0;
        end else begin
            unique case (state_q)
                StIdle: begin
                    multisim_server_start("loom_e2e_test");
                    state_q <= StServerStart;
                end

                StServerStart: begin
                    // Capture first pair of operands from LFSR
                    arg_a_q <= lfsr_q;
                    lfsr_q  <= lfsr_next;
                    state_q <= StCallAdd;
                end

                StCallAdd: begin
                    // Latch second operand, then DPI call
                    arg_b_q  <= lfsr_q;
                    lfsr_q   <= lfsr_next;
                    result_q <= dpi_add({16'd0, arg_a_q}, {16'd0, lfsr_q});
                    state_q  <= StCheck;
                end

                StCheck: begin
                    $display("Got result %x", result_q);
                    if (result_q == expected) begin
                        n_pass_q <= n_pass_q + 4'd1;
                    end else begin
                        n_fail_q <= n_fail_q + 4'd1;
                    end
                    state_q <= StNext;
                end

                StNext: begin
                    if (iter_q == N_ITER - 1) begin
                        state_q <= StReport;
                    end else begin
                        iter_q  <= iter_q + 4'd1;
                        arg_a_q <= lfsr_q;
                        lfsr_q  <= lfsr_next;
                        state_q <= StCallAdd;
                    end
                end

                StReport: begin
                    result_q <= dpi_report_result({28'd0, n_pass_q}, {28'd0, n_fail_q});
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
