// SPDX-License-Identifier: Apache-2.0
// Minimal DUT for AXI-Lite completeness test
//
// Simple counter — all interesting testing happens at the infrastructure
// level (demux, emu_ctrl, dpi_regfile, scan_ctrl address map).

module axil_completeness (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [31:0] count_o
);

    logic [31:0] counter_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            counter_q <= 32'd0;
        end else begin
            counter_q <= counter_q + 32'd1;
        end
    end

    assign count_o = counter_q;

endmodule
