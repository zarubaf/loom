// SPDX-License-Identifier: Apache-2.0
// DPI Test Module with actual DPI imports
//
// This module uses real DPI-C imports that yosys-slang will transform
// into $__loom_dpi_call cells, which dpi_bridge then converts to
// hardware interfaces.
//
// Ports: Only clk_i and rst_ni - all test values are internal constants.
// Simplified to single DPI function for debugging.

// DPI function declaration - single function to avoid multi-driver issues
import "DPI-C" function int dpi_add(input int a, input int b);

module dpi_test (
    input  logic clk_i,
    input  logic rst_ni
);

    // Test parameters - internal constants
    localparam logic [31:0] TEST_ARG_A = 32'd42;
    localparam logic [31:0] TEST_ARG_B = 32'd17;
    localparam logic [31:0] EXPECTED_RESULT = TEST_ARG_A + TEST_ARG_B;  // 59

    // State machine
    typedef enum logic [1:0] {
        StIdle,
        StCallAdd,
        StCheckResult,
        StDone
    } state_e;

    state_e state_q, state_d;
    logic [31:0] result_q, result_d;
    logic test_passed_q, test_passed_d;

    // Sequential logic
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q       <= StIdle;
            result_q      <= 32'd0;
            test_passed_q <= 1'b0;
        end else begin
            state_q       <= state_d;
            result_q      <= result_d;
            test_passed_q <= test_passed_d;
        end
    end

    // Combinational next-state logic
    always_comb begin
        // Defaults
        state_d       = state_q;
        result_d      = result_q;
        test_passed_d = test_passed_q;

        unique case (state_q)
            StIdle: begin
                state_d = StCallAdd;
            end

            StCallAdd: begin
                // Call DPI function - yosys-slang creates $__loom_dpi_call cell here
                result_d = dpi_add(TEST_ARG_A, TEST_ARG_B);
                state_d = StCheckResult;
            end

            StCheckResult: begin
                // Check result and mark done
                test_passed_d = (result_q == EXPECTED_RESULT);
                state_d = StDone;
            end

            StDone: begin
                // Stay in done state
                state_d = StDone;
            end

            default: state_d = StIdle;
        endcase
    end

endmodule
