// SPDX-License-Identifier: Apache-2.0
// counter.sv - Simple counter for scan chain testing
//
// Multiple counters with different widths to test scan capture/restore

module counter #(
    parameter int WIDTH = 8
)(
    input  logic             clk,
    input  logic             rst,

    // Control
    input  logic             enable,
    input  logic             load,
    input  logic [WIDTH-1:0] load_value,

    // Status
    output logic [WIDTH-1:0] count,
    output logic             overflow
);

    // Internal state
    logic [WIDTH-1:0] counter;
    logic [WIDTH-1:0] prev_counter;
    logic             wrapped;

    // Counter logic
    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            counter <= '0;
            prev_counter <= '0;
            wrapped <= 1'b0;
        end else begin
            prev_counter <= counter;

            if (load) begin
                counter <= load_value;
                wrapped <= 1'b0;
            end else if (enable) begin
                if (counter == {WIDTH{1'b1}}) begin
                    wrapped <= 1'b1;
                end
                counter <= counter + 1;
            end
        end
    end

    assign count = counter;
    assign overflow = wrapped;

endmodule

// Top module with multiple counters for more state
module multi_counter (
    input  logic        clk,
    input  logic        rst,

    input  logic        en_a,
    input  logic        en_b,
    input  logic        load_a,
    input  logic        load_b,
    input  logic [7:0]  load_val_a,
    input  logic [15:0] load_val_b,

    output logic [7:0]  count_a,
    output logic [15:0] count_b,
    output logic        overflow_a,
    output logic        overflow_b
);

    counter #(.WIDTH(8)) counter_a (
        .clk        (clk),
        .rst        (rst),
        .enable     (en_a),
        .load       (load_a),
        .load_value (load_val_a),
        .count      (count_a),
        .overflow   (overflow_a)
    );

    counter #(.WIDTH(16)) counter_b (
        .clk        (clk),
        .rst        (rst),
        .enable     (en_b),
        .load       (load_b),
        .load_value (load_val_b),
        .count      (count_b),
        .overflow   (overflow_b)
    );

endmodule
