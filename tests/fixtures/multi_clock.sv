// SPDX-License-Identifier: Apache-2.0
// multi_clock.sv - Test module with multiple clock domains
// Tests scan chain handling of different clock domains

module multi_clock (
    input  logic       clk_a,
    input  logic       clk_b,
    input  logic       rst,
    input  logic [7:0] data_a,
    input  logic [7:0] data_b,
    output logic [7:0] out_a,
    output logic [7:0] out_b
);
    // Clock domain A
    logic [7:0] reg_a1, reg_a2;

    always_ff @(posedge clk_a or posedge rst) begin
        if (rst) begin
            reg_a1 <= 8'h0;
            reg_a2 <= 8'h0;
        end else begin
            reg_a1 <= data_a;
            reg_a2 <= reg_a1;
        end
    end

    assign out_a = reg_a2;

    // Clock domain B
    logic [7:0] reg_b1, reg_b2;

    always_ff @(posedge clk_b or posedge rst) begin
        if (rst) begin
            reg_b1 <= 8'h0;
            reg_b2 <= 8'h0;
        end else begin
            reg_b1 <= data_b;
            reg_b2 <= reg_b1;
        end
    end

    assign out_b = reg_b2;
endmodule
