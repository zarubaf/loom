// hierarchy.sv - Test module with nested hierarchy
// Tests scan chain insertion across module boundaries

module inner_reg (
    input  logic       clk,
    input  logic       rst,
    input  logic [3:0] d,
    output logic [3:0] q
);
    always_ff @(posedge clk or posedge rst) begin
        if (rst)
            q <= 4'h0;
        else
            q <= d;
    end
endmodule

module middle_level (
    input  logic       clk,
    input  logic       rst,
    input  logic [7:0] data_in,
    output logic [7:0] data_out
);
    logic [3:0] stage1_out;

    inner_reg stage1 (
        .clk(clk),
        .rst(rst),
        .d(data_in[3:0]),
        .q(stage1_out)
    );

    inner_reg stage2 (
        .clk(clk),
        .rst(rst),
        .d(data_in[7:4]),
        .q(data_out[7:4])
    );

    assign data_out[3:0] = stage1_out;
endmodule

module hierarchy (
    input  logic        clk,
    input  logic        rst,
    input  logic [15:0] data_in,
    output logic [15:0] data_out
);
    logic [7:0] mid_out;

    middle_level mid1 (
        .clk(clk),
        .rst(rst),
        .data_in(data_in[7:0]),
        .data_out(mid_out)
    );

    // Top-level register
    logic [7:0] top_reg;
    always_ff @(posedge clk or posedge rst) begin
        if (rst)
            top_reg <= 8'h0;
        else
            top_reg <= data_in[15:8];
    end

    assign data_out = {top_reg, mid_out};
endmodule
