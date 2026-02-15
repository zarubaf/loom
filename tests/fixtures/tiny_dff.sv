// tiny_dff.sv - Minimal test module with flip-flops
// Used to test scan chain insertion

module tiny_dff (
    input  logic        clk,
    input  logic        rst,
    input  logic [7:0]  data_in,
    output logic [7:0]  data_out,
    output logic        flag
);
    logic [7:0] reg_a;
    logic       reg_b;

    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            reg_a <= 8'h0;
            reg_b <= 1'b0;
        end else begin
            reg_a <= data_in;
            reg_b <= |data_in;
        end
    end

    assign data_out = reg_a;
    assign flag = reg_b;
endmodule
