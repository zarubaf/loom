// SPDX-License-Identifier: Apache-2.0
// DUT for reset_extract pass test
//
// Contains all FF types with both zero and non-zero reset values:
//   - Async reset (posedge clk or negedge rst_ni) → $adff
//   - Async reset + enable → $adffe
//   - Sync reset (posedge clk, if (!rst_ni)) → $sdff
//   - Sync reset + enable → $sdffe
//   - No-reset FF → $dff

module reset_extract_test (
    input  logic       clk_i,
    input  logic       rst_ni,
    input  logic       en_i,
    input  logic [7:0] data_i,
    output logic [7:0] async_zero_o,
    output logic [7:0] async_nonzero_o,
    output logic [7:0] asynce_zero_o,
    output logic [7:0] asynce_nonzero_o,
    output logic [7:0] sync_zero_o,
    output logic [7:0] sync_nonzero_o,
    output logic [7:0] synce_zero_o,
    output logic [7:0] synce_nonzero_o,
    output logic [7:0] norst_o
);

    // Async reset (becomes $adff) — reset to 0
    logic [7:0] async_zero_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            async_zero_q <= 8'd0;
        else
            async_zero_q <= data_i;
    end

    // Async reset (becomes $adff) — reset to non-zero
    logic [7:0] async_nonzero_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            async_nonzero_q <= 8'hAB;
        else
            async_nonzero_q <= data_i;
    end

    // Async reset + enable (becomes $adffe) — reset to 0
    logic [7:0] asynce_zero_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            asynce_zero_q <= 8'd0;
        else if (en_i)
            asynce_zero_q <= data_i;
    end

    // Async reset + enable (becomes $adffe) — reset to non-zero
    logic [7:0] asynce_nonzero_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            asynce_nonzero_q <= 8'hCD;
        else if (en_i)
            asynce_nonzero_q <= data_i;
    end

    // Sync reset (becomes $sdff) — reset to 0
    logic [7:0] sync_zero_q;
    always_ff @(posedge clk_i) begin
        if (!rst_ni)
            sync_zero_q <= 8'd0;
        else
            sync_zero_q <= data_i;
    end

    // Sync reset (becomes $sdff) — reset to non-zero
    logic [7:0] sync_nonzero_q;
    always_ff @(posedge clk_i) begin
        if (!rst_ni)
            sync_nonzero_q <= 8'hEF;
        else
            sync_nonzero_q <= data_i;
    end

    // Sync reset + enable (becomes $sdffe) — reset to 0
    logic [7:0] synce_zero_q;
    always_ff @(posedge clk_i) begin
        if (!rst_ni)
            synce_zero_q <= 8'd0;
        else if (en_i)
            synce_zero_q <= data_i;
    end

    // Sync reset + enable (becomes $sdffe) — reset to non-zero
    logic [7:0] synce_nonzero_q;
    always_ff @(posedge clk_i) begin
        if (!rst_ni)
            synce_nonzero_q <= 8'h12;
        else if (en_i)
            synce_nonzero_q <= data_i;
    end

    // No-reset FF (becomes $dff)
    logic [7:0] norst_q;
    always_ff @(posedge clk_i) begin
        norst_q <= data_i;
    end

    assign async_zero_o    = async_zero_q;
    assign async_nonzero_o = async_nonzero_q;
    assign asynce_zero_o   = asynce_zero_q;
    assign asynce_nonzero_o = asynce_nonzero_q;
    assign sync_zero_o     = sync_zero_q;
    assign sync_nonzero_o  = sync_nonzero_q;
    assign synce_zero_o    = synce_zero_q;
    assign synce_nonzero_o = synce_nonzero_q;
    assign norst_o         = norst_q;

endmodule
