// SPDX-License-Identifier: Apache-2.0
// Loom AXI-Lite 1:N Demux
//
// Parameterizable address decoder that routes AXI-Lite transactions to N slaves.
// Address decode: route to master i when (addr & ADDR_MASK[i]) == BASE_ADDR[i].
// Checked in order 0..N-1; first match wins.

`timescale 1ns/1ps

module loom_axil_demux #(
    parameter int unsigned ADDR_WIDTH = 20,
    parameter int unsigned N_MASTERS  = 3,
    parameter logic [N_MASTERS-1:0][ADDR_WIDTH-1:0] BASE_ADDR = '0,
    parameter logic [N_MASTERS-1:0][ADDR_WIDTH-1:0] ADDR_MASK = '0
)(
    input  logic clk_i,
    input  logic rst_ni,

    // AXI-Lite Slave port (from single master)
    input  logic [ADDR_WIDTH-1:0]   s_axil_araddr_i,
    input  logic                    s_axil_arvalid_i,
    output logic                    s_axil_arready_o,
    output logic [31:0]             s_axil_rdata_o,
    output logic [1:0]              s_axil_rresp_o,
    output logic                    s_axil_rvalid_o,
    input  logic                    s_axil_rready_i,

    input  logic [ADDR_WIDTH-1:0]   s_axil_awaddr_i,
    input  logic                    s_axil_awvalid_i,
    output logic                    s_axil_awready_o,
    input  logic [31:0]             s_axil_wdata_i,
    input  logic [3:0]              s_axil_wstrb_i,
    input  logic                    s_axil_wvalid_i,
    output logic                    s_axil_wready_o,
    output logic [1:0]              s_axil_bresp_o,
    output logic                    s_axil_bvalid_o,
    input  logic                    s_axil_bready_i,

    // Master ports (to N slaves) — flat arrays of width N*signal_width
    output logic [N_MASTERS*ADDR_WIDTH-1:0] m_axil_araddr_o,
    output logic [N_MASTERS-1:0]            m_axil_arvalid_o,
    input  logic [N_MASTERS-1:0]            m_axil_arready_i,
    input  logic [N_MASTERS*32-1:0]         m_axil_rdata_i,
    input  logic [N_MASTERS*2-1:0]          m_axil_rresp_i,
    input  logic [N_MASTERS-1:0]            m_axil_rvalid_i,
    output logic [N_MASTERS-1:0]            m_axil_rready_o,

    output logic [N_MASTERS*ADDR_WIDTH-1:0] m_axil_awaddr_o,
    output logic [N_MASTERS-1:0]            m_axil_awvalid_o,
    input  logic [N_MASTERS-1:0]            m_axil_awready_i,
    output logic [N_MASTERS*32-1:0]         m_axil_wdata_o,
    output logic [N_MASTERS*4-1:0]          m_axil_wstrb_o,
    output logic [N_MASTERS-1:0]            m_axil_wvalid_o,
    input  logic [N_MASTERS-1:0]            m_axil_wready_i,
    input  logic [N_MASTERS*2-1:0]          m_axil_bresp_i,
    input  logic [N_MASTERS-1:0]            m_axil_bvalid_i,
    output logic [N_MASTERS-1:0]            m_axil_bready_o
);

    // =========================================================================
    // Address Decode
    // =========================================================================

    function automatic logic [$clog2(N_MASTERS+1)-1:0] decode_slave(
        logic [ADDR_WIDTH-1:0] addr
    );
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            if ((addr & ADDR_MASK[i]) == BASE_ADDR[i])
                return i[$clog2(N_MASTERS+1)-1:0];
        end
        return N_MASTERS[$clog2(N_MASTERS+1)-1:0]; // no match
    endfunction

    localparam int SEL_W = $clog2(N_MASTERS + 1);
    // Master index width — enough bits for 0..N_MASTERS-1
    localparam int IDX_W = N_MASTERS > 1 ? $clog2(N_MASTERS) : 1;

    // =========================================================================
    // Read Path
    // =========================================================================

    logic [SEL_W-1:0] rd_slave_sel;
    logic [SEL_W-1:0] rd_slave_active_q;
    logic              rd_in_progress_q;

    always_comb begin
        rd_slave_sel = decode_slave(s_axil_araddr_i);
    end

    // Address routing — subtract BASE_ADDR to give each slave base-relative addresses
    always_comb begin
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            m_axil_araddr_o[i*ADDR_WIDTH +: ADDR_WIDTH] = s_axil_araddr_i - BASE_ADDR[i];
            m_axil_arvalid_o[i] = 1'b0;
        end
        if (s_axil_arvalid_i && !rd_in_progress_q &&
            rd_slave_sel < N_MASTERS[SEL_W-1:0]) begin
            m_axil_arvalid_o[rd_slave_sel[IDX_W-1:0]] = 1'b1;
        end
    end

    // Ready mux
    always_comb begin
        if (!rd_in_progress_q && rd_slave_sel < N_MASTERS[SEL_W-1:0])
            s_axil_arready_o = m_axil_arready_i[rd_slave_sel[IDX_W-1:0]];
        else
            s_axil_arready_o = 1'b0;
    end

    // Track active read slave
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_in_progress_q  <= 1'b0;
            rd_slave_active_q <= '0;
        end else begin
            if (s_axil_arvalid_i && s_axil_arready_o) begin
                rd_in_progress_q  <= 1'b1;
                rd_slave_active_q <= rd_slave_sel;
            end
            if (s_axil_rvalid_o && s_axil_rready_i) begin
                rd_in_progress_q <= 1'b0;
            end
        end
    end

    // Response mux
    always_comb begin
        s_axil_rdata_o  = 32'hDEAD_BEEF;
        s_axil_rresp_o  = 2'b10; // SLVERR
        s_axil_rvalid_o = 1'b0;
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            if (rd_slave_active_q == i[SEL_W-1:0]) begin
                s_axil_rdata_o  = m_axil_rdata_i[i*32 +: 32];
                s_axil_rresp_o  = m_axil_rresp_i[i*2 +: 2];
                s_axil_rvalid_o = m_axil_rvalid_i[i];
            end
        end
    end

    // Read ready routing
    always_comb begin
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            m_axil_rready_o[i] = (rd_slave_active_q == i[SEL_W-1:0]) ?
                                  s_axil_rready_i : 1'b0;
        end
    end

    // =========================================================================
    // Write Path
    // =========================================================================

    logic [SEL_W-1:0] wr_slave_sel;
    logic [SEL_W-1:0] wr_slave_active_q;
    logic              wr_in_progress_q;

    always_comb begin
        wr_slave_sel = decode_slave(s_axil_awaddr_i);
    end

    // Address routing — subtract BASE_ADDR to give each slave base-relative addresses
    always_comb begin
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            m_axil_awaddr_o[i*ADDR_WIDTH +: ADDR_WIDTH] = s_axil_awaddr_i - BASE_ADDR[i];
            m_axil_awvalid_o[i] = 1'b0;
        end
        if (s_axil_awvalid_i && !wr_in_progress_q &&
            wr_slave_sel < N_MASTERS[SEL_W-1:0]) begin
            m_axil_awvalid_o[wr_slave_sel[IDX_W-1:0]] = 1'b1;
        end
    end

    // Data routing
    always_comb begin
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            m_axil_wdata_o[i*32 +: 32] = s_axil_wdata_i;
            m_axil_wstrb_o[i*4 +: 4]   = s_axil_wstrb_i;
            m_axil_wvalid_o[i] = 1'b0;
        end
        if (s_axil_wvalid_i && !wr_in_progress_q &&
            wr_slave_sel < N_MASTERS[SEL_W-1:0]) begin
            m_axil_wvalid_o[wr_slave_sel[IDX_W-1:0]] = 1'b1;
        end
    end

    // Ready mux
    always_comb begin
        if (!wr_in_progress_q && wr_slave_sel < N_MASTERS[SEL_W-1:0]) begin
            s_axil_awready_o = m_axil_awready_i[wr_slave_sel[IDX_W-1:0]];
            s_axil_wready_o  = m_axil_wready_i[wr_slave_sel[IDX_W-1:0]];
        end else begin
            s_axil_awready_o = 1'b0;
            s_axil_wready_o  = 1'b0;
        end
    end

    // Track active write slave
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_in_progress_q  <= 1'b0;
            wr_slave_active_q <= '0;
        end else begin
            if (s_axil_awvalid_i && s_axil_awready_o) begin
                wr_in_progress_q  <= 1'b1;
                wr_slave_active_q <= wr_slave_sel;
            end
            if (s_axil_bvalid_o && s_axil_bready_i) begin
                wr_in_progress_q <= 1'b0;
            end
        end
    end

    // Response mux
    always_comb begin
        s_axil_bresp_o  = 2'b10; // SLVERR
        s_axil_bvalid_o = 1'b0;
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            if (wr_slave_active_q == i[SEL_W-1:0]) begin
                s_axil_bresp_o  = m_axil_bresp_i[i*2 +: 2];
                s_axil_bvalid_o = m_axil_bvalid_i[i];
            end
        end
    end

    // Write response ready routing
    always_comb begin
        for (int i = 0; i < int'(N_MASTERS); i++) begin
            m_axil_bready_o[i] = (wr_slave_active_q == i[SEL_W-1:0]) ?
                                  s_axil_bready_i : 1'b0;
        end
    end

endmodule
