// SPDX-License-Identifier: Apache-2.0
// Simple DUT with a memory for emu_top integration testing.

module mem_dut (
    input  logic       clk_i,
    input  logic       rst_ni,
    input  logic       we_i,
    input  logic [3:0] addr_i,
    input  logic [7:0] wdata_i,
    output logic [7:0] rdata_o,
    output logic [7:0] count_o
);

    // 8-bit x 16 memory
    logic [7:0] mem [0:15];

    always_ff @(posedge clk_i) begin
        if (we_i)
            mem[addr_i] <= wdata_i;
        rdata_o <= mem[addr_i];
    end

    // Standalone register with async reset so reset_extract has something to strip
    logic [7:0] counter_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            counter_q <= 8'h0;
        else
            counter_q <= counter_q + 8'h1;
    end
    assign count_o = counter_q;

endmodule
