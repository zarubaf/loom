// SPDX-License-Identifier: Apache-2.0
// Loom AXI4 Error Slave
//
// Accepts every AXI4 transaction and responds with a configurable error
// (DECERR by default).  Inspired by pulp-platform/axi axi_err_slv.sv.
//
// Read path:  accept AR, then send arlen+1 R beats with error, rlast on
//             the final beat.
// Write path: accept AW, eat W beats until wlast, then send B response.
//
// Both channels operate independently so a read burst can be in-flight
// while a write is being consumed.  Only one outstanding transaction per
// channel (arready/awready deasserted while busy).

module loom_axi4_err_slv #(
    parameter int unsigned ID_WIDTH   = 4,
    parameter int unsigned DATA_WIDTH = 128,
    parameter logic [1:0]  RESP       = 2'b11,        // DECERR
    parameter logic [DATA_WIDTH-1:0] RESP_DATA = '0   // filler for R data
)(
    input  logic clk_i,
    input  logic rst_ni,

    // AXI4 Slave — Write Address
    input  logic [ID_WIDTH-1:0]   s_axi_awid,
    input  logic [63:0]           s_axi_awaddr,
    input  logic [7:0]            s_axi_awlen,
    input  logic [2:0]            s_axi_awsize,
    input  logic [1:0]            s_axi_awburst,
    input  logic                  s_axi_awlock,
    input  logic [3:0]            s_axi_awcache,
    input  logic [2:0]            s_axi_awprot,
    input  logic                  s_axi_awvalid,
    output logic                  s_axi_awready,

    // AXI4 Slave — Write Data
    input  logic [DATA_WIDTH-1:0] s_axi_wdata,
    input  logic [DATA_WIDTH/8-1:0] s_axi_wstrb,
    input  logic                  s_axi_wlast,
    input  logic                  s_axi_wvalid,
    output logic                  s_axi_wready,

    // AXI4 Slave — Write Response
    output logic [ID_WIDTH-1:0]   s_axi_bid,
    output logic [1:0]            s_axi_bresp,
    output logic                  s_axi_bvalid,
    input  logic                  s_axi_bready,

    // AXI4 Slave — Read Address
    input  logic [ID_WIDTH-1:0]   s_axi_arid,
    input  logic [63:0]           s_axi_araddr,
    input  logic [7:0]            s_axi_arlen,
    input  logic [2:0]            s_axi_arsize,
    input  logic [1:0]            s_axi_arburst,
    input  logic                  s_axi_arlock,
    input  logic [3:0]            s_axi_arcache,
    input  logic [2:0]            s_axi_arprot,
    input  logic                  s_axi_arvalid,
    output logic                  s_axi_arready,

    // AXI4 Slave — Read Data
    output logic [ID_WIDTH-1:0]   s_axi_rid,
    output logic [DATA_WIDTH-1:0] s_axi_rdata,
    output logic [1:0]            s_axi_rresp,
    output logic                  s_axi_rlast,
    output logic                  s_axi_rvalid,
    input  logic                  s_axi_rready
);

    // =========================================================================
    // Read Channel
    // =========================================================================

    typedef enum logic [1:0] {
        StRdIdle,
        StRdBurst
    } rd_state_e;

    rd_state_e          rd_state_d, rd_state_q;
    logic [ID_WIDTH-1:0] rd_id_d,   rd_id_q;
    logic [7:0]         rd_cnt_d,   rd_cnt_q;  // beats remaining (0 = last)

    always_comb begin
        rd_state_d = rd_state_q;
        rd_id_d    = rd_id_q;
        rd_cnt_d   = rd_cnt_q;

        s_axi_arready = 1'b0;
        s_axi_rvalid  = 1'b0;
        s_axi_rlast   = 1'b0;
        s_axi_rid     = rd_id_q;
        s_axi_rdata   = RESP_DATA;
        s_axi_rresp   = RESP;

        unique case (rd_state_q)
            StRdIdle: begin
                s_axi_arready = 1'b1;
                if (s_axi_arvalid) begin
                    rd_id_d    = s_axi_arid;
                    rd_cnt_d   = s_axi_arlen;
                    rd_state_d = StRdBurst;
                end
            end

            StRdBurst: begin
                s_axi_rvalid = 1'b1;
                s_axi_rlast  = (rd_cnt_q == 8'd0);
                if (s_axi_rready) begin
                    if (rd_cnt_q == 8'd0) begin
                        rd_state_d = StRdIdle;
                    end else begin
                        rd_cnt_d = rd_cnt_q - 8'd1;
                    end
                end
            end

            default: rd_state_d = StRdIdle;
        endcase
    end

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_state_q <= StRdIdle;
            rd_id_q    <= '0;
            rd_cnt_q   <= 8'd0;
        end else begin
            rd_state_q <= rd_state_d;
            rd_id_q    <= rd_id_d;
            rd_cnt_q   <= rd_cnt_d;
        end
    end

    // =========================================================================
    // Write Channel
    // =========================================================================

    typedef enum logic [1:0] {
        StWrIdle,
        StWrEat,
        StWrResp
    } wr_state_e;

    wr_state_e          wr_state_d, wr_state_q;
    logic [ID_WIDTH-1:0] wr_id_d,   wr_id_q;

    always_comb begin
        wr_state_d = wr_state_q;
        wr_id_d    = wr_id_q;

        s_axi_awready = 1'b0;
        s_axi_wready  = 1'b0;
        s_axi_bvalid  = 1'b0;
        s_axi_bid     = wr_id_q;
        s_axi_bresp   = RESP;

        unique case (wr_state_q)
            StWrIdle: begin
                s_axi_awready = 1'b1;
                if (s_axi_awvalid) begin
                    wr_id_d    = s_axi_awid;
                    wr_state_d = StWrEat;
                end
            end

            StWrEat: begin
                s_axi_wready = 1'b1;
                if (s_axi_wvalid && s_axi_wlast) begin
                    wr_state_d = StWrResp;
                end
            end

            StWrResp: begin
                s_axi_bvalid = 1'b1;
                if (s_axi_bready) begin
                    wr_state_d = StWrIdle;
                end
            end

            default: wr_state_d = StWrIdle;
        endcase
    end

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_state_q <= StWrIdle;
            wr_id_q    <= '0;
        end else begin
            wr_state_q <= wr_state_d;
            wr_id_q    <= wr_id_d;
        end
    end

endmodule
