// SPDX-License-Identifier: Apache-2.0
// DPI Test Module with actual DPI imports
//
// This module uses real DPI-C imports that yosys-slang will transform
// into $__loom_dpi_call cells, which dpi_bridge then converts to
// hardware interfaces.
//
// IMPORTANT: DPI calls must be in clocked (always_ff) blocks only.
// This ensures deterministic clock gating behavior.

// DPI function declarations
import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function int dpi_report_result(input int passed, input int result);

module dpi_test (
    input  logic clk_i,
    input  logic rst_ni
);

    // Test parameters - internal constants
    localparam logic [31:0] TEST_ARG_A = 32'd42;
    localparam logic [31:0] TEST_ARG_B = 32'd17;
    localparam logic [31:0] EXPECTED_RESULT = TEST_ARG_A + TEST_ARG_B;  // 59

    // State machine
    typedef enum logic [2:0] {
        StIdle,
        StCallAdd,
        StCheckResult,
        StReport,
        StDone
    } state_e;

    state_e state_q;
    logic [31:0] result_q;
    logic test_passed_q;

    // Sequential logic with DPI calls in clocked block
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q       <= StIdle;
            result_q      <= 32'd0;
            test_passed_q <= 1'b0;
        end else begin
            unique case (state_q)
                StIdle: begin
                    state_q <= StCallAdd;
                end

                StCallAdd: begin
                    // DPI call in clocked block - triggers clock gating
                    result_q <= dpi_add(TEST_ARG_A, TEST_ARG_B);
                    state_q <= StCheckResult;
                end

                StCheckResult: begin
                    // Check result
                    test_passed_q <= (result_q == EXPECTED_RESULT);
                    state_q <= StReport;
                end

                StReport: begin
                    // DPI call to report result
                    result_q <= dpi_report_result({31'b0, test_passed_q}, result_q);
                    state_q <= StDone;
                end

                StDone: begin
                    // Stay in done state
                    state_q <= StDone;
                    $finish;
                end

                default: state_q <= StIdle;
            endcase
        end
    end

endmodule
