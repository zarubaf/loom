// SPDX-License-Identifier: Apache-2.0
// DUT for initial/reset DPI call test
//
// Tests two DPI call patterns:
//   1. Void DPI in initial block (side effect only)
//   2. DPI in reset block (inject return value into scan chain)

import "DPI-C" function void init_setup(input string tag);
import "DPI-C" function int get_init_val(input int seed);

module initial_dpi_test (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [31:0] value_o,
    output logic [7:0]  counter_o
);
    // Case 1: Void DPI in initial block (side effect only)
    initial begin
        init_setup("test_tag");
    end

    // Case 2: DPI in reset block (inject into scan chain)
    logic [31:0] init_reg_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            init_reg_q <= get_init_val(42);
        else
            init_reg_q <= init_reg_q + 1;
    end

    // Simple counter (to verify design still runs after init)
    logic [7:0] counter_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            counter_q <= 8'h00;
        else
            counter_q <= counter_q + 1;
    end

    assign value_o = init_reg_q;
    assign counter_o = counter_q;
endmodule
