// SPDX-License-Identifier: Apache-2.0
// Test design with hierarchical memories for mem_shadow pass testing
//
// This module contains:
// - A simple 8-bit x 256 memory (inferred as $mem_v2) at top level
// - A submodule with an instantiated sram (hierarchy test)

// Submodule containing an sram instance
module mem_subsystem (
    input  logic        clk,
    input  logic        rst_n,

    // Memory interface (16-bit x 64)
    input  logic        we,
    input  logic [5:0]  addr,
    input  logic [15:0] wdata,
    output logic [15:0] rdata
);

    // Instantiate sram from fixtures
    sram #(
        .Depth(64),
        .DataWidth(16),
        .ByteWidth(8)
    ) u_sram (
        .clk_i(clk),
        .req_i(1'b1),           // Always enabled
        .we_i(we),
        .addr_i(addr),
        .be_i(2'b11),           // All bytes enabled
        .wdata_i(wdata),
        .rdata_o1(rdata)
    );

endmodule

module mem_test (
    input  logic        clk,
    input  logic        rst_n,

    // Memory A interface (8-bit x 256) - inferred at top level
    input  logic        mem_a_we,
    input  logic [7:0]  mem_a_addr,
    input  logic [7:0]  mem_a_wdata,
    output logic [7:0]  mem_a_rdata,

    // Memory B interface (16-bit x 64) - in submodule
    input  logic        mem_b_we,
    input  logic [5:0]  mem_b_addr,
    input  logic [15:0] mem_b_wdata,
    output logic [15:0] mem_b_rdata
);

    // Memory A: 8-bit x 256 deep (inferred, top-level)
    logic [7:0] mem_a [0:255];

    always_ff @(posedge clk) begin
        if (mem_a_we) begin
            mem_a[mem_a_addr] <= mem_a_wdata;
        end
        mem_a_rdata <= mem_a[mem_a_addr];
    end

    // Memory B: instantiated in submodule (hierarchy)
    mem_subsystem u_mem_b (
        .clk(clk),
        .rst_n(rst_n),
        .we(mem_b_we),
        .addr(mem_b_addr),
        .wdata(mem_b_wdata),
        .rdata(mem_b_rdata)
    );

endmodule
