// SPDX-License-Identifier: Apache-2.0
// Loom AXI-Lite Interconnect
//
// Simple address decoder that routes AXI-Lite transactions to the correct slave.
// Address map (20-bit address space = 1MB BAR):
//   0x0_0000 - 0x0_00FF  -> slave 0: emu_ctrl
//   0x0_0100 - 0x0_FFFF  -> slave 1: dpi_regfile
//   0x1_0000 - 0x1_FFFF  -> slave 2: mem_ctrl (reserved)
//   0x2_0000 - 0x2_FFFF  -> slave 3: scan_ctrl (reserved)

`timescale 1ns/1ps

module loom_axi_interconnect #(
    parameter int unsigned ADDR_WIDTH = 20
)(
    input  logic                    clk_i,
    input  logic                    rst_ni,

    // AXI-Lite Master (from BFM or XDMA)
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

    // Slave 0: emu_ctrl (0x0_0000 - 0x0_00FF)
    output logic [7:0]              m0_axil_araddr_o,
    output logic                    m0_axil_arvalid_o,
    input  logic                    m0_axil_arready_i,
    input  logic [31:0]             m0_axil_rdata_i,
    input  logic [1:0]              m0_axil_rresp_i,
    input  logic                    m0_axil_rvalid_i,
    output logic                    m0_axil_rready_o,

    output logic [7:0]              m0_axil_awaddr_o,
    output logic                    m0_axil_awvalid_o,
    input  logic                    m0_axil_awready_i,
    output logic [31:0]             m0_axil_wdata_o,
    output logic                    m0_axil_wvalid_o,
    input  logic                    m0_axil_wready_i,
    input  logic [1:0]              m0_axil_bresp_i,
    input  logic                    m0_axil_bvalid_i,
    output logic                    m0_axil_bready_o,

    // Slave 1: dpi_regfile (0x0_0100 - 0x0_FFFF)
    output logic [15:0]             m1_axil_araddr_o,
    output logic                    m1_axil_arvalid_o,
    input  logic                    m1_axil_arready_i,
    input  logic [31:0]             m1_axil_rdata_i,
    input  logic [1:0]              m1_axil_rresp_i,
    input  logic                    m1_axil_rvalid_i,
    output logic                    m1_axil_rready_o,

    output logic [15:0]             m1_axil_awaddr_o,
    output logic                    m1_axil_awvalid_o,
    input  logic                    m1_axil_awready_i,
    output logic [31:0]             m1_axil_wdata_o,
    output logic                    m1_axil_wvalid_o,
    input  logic                    m1_axil_wready_i,
    input  logic [1:0]              m1_axil_bresp_i,
    input  logic                    m1_axil_bvalid_i,
    output logic                    m1_axil_bready_o,

    // Slave 2: scan_ctrl (0x2_0000 - 0x2_FFFF)
    output logic [11:0]             m2_axil_araddr_o,
    output logic                    m2_axil_arvalid_o,
    input  logic                    m2_axil_arready_i,
    input  logic [31:0]             m2_axil_rdata_i,
    input  logic [1:0]              m2_axil_rresp_i,
    input  logic                    m2_axil_rvalid_i,
    output logic                    m2_axil_rready_o,

    output logic [11:0]             m2_axil_awaddr_o,
    output logic                    m2_axil_awvalid_o,
    input  logic                    m2_axil_awready_i,
    output logic [31:0]             m2_axil_wdata_o,
    output logic                    m2_axil_wvalid_o,
    input  logic                    m2_axil_wready_i,
    input  logic [1:0]              m2_axil_bresp_i,
    input  logic                    m2_axil_bvalid_i,
    output logic                    m2_axil_bready_o
);

    // =========================================================================
    // Address Decode
    // =========================================================================
    // Bits [19:16] select major region
    // For region 0 (0x0xxxx), bits [15:8] further decode:
    //   0x00 -> emu_ctrl
    //   0x01+ -> dpi_regfile

    function automatic logic [1:0] decode_slave(logic [ADDR_WIDTH-1:0] addr);
        if (addr[19:16] == 4'h0) begin
            if (addr[15:8] == 8'h00)
                return 2'd0;  // emu_ctrl
            else
                return 2'd1;  // dpi_regfile
        end else if (addr[19:16] == 4'h2) begin
            return 2'd2;  // scan_ctrl
        end else begin
            // Reserved for mem_ctrl (0x1xxxx)
            return 2'd1;  // Default to dpi_regfile
        end
    endfunction

    // =========================================================================
    // Read Path
    // =========================================================================

    logic [1:0] rd_slave_sel;
    logic [1:0] rd_slave_active_q;
    logic       rd_in_progress_q;

    always_comb begin
        rd_slave_sel = decode_slave(s_axil_araddr_i);
    end

    // Address routing
    // Note: For dpi_regfile (m1), subtract 0x100 to get relative addresses
    // Note: For scan_ctrl (m2), subtract 0x20000 to get relative addresses
    always_comb begin
        // Default: no slave selected
        m0_axil_araddr_o  = s_axil_araddr_i[7:0];
        m0_axil_arvalid_o = 1'b0;
        m1_axil_araddr_o  = s_axil_araddr_i[15:0] - 16'h0100;  // Subtract base
        m1_axil_arvalid_o = 1'b0;
        m2_axil_araddr_o  = s_axil_araddr_i[11:0];  // Just use low bits
        m2_axil_arvalid_o = 1'b0;

        if (s_axil_arvalid_i && !rd_in_progress_q) begin
            case (rd_slave_sel)
                2'd0: m0_axil_arvalid_o = 1'b1;
                2'd1: m1_axil_arvalid_o = 1'b1;
                2'd2: m2_axil_arvalid_o = 1'b1;
                default: ;
            endcase
        end
    end

    // Ready mux
    always_comb begin
        case (rd_slave_sel)
            2'd0:    s_axil_arready_o = m0_axil_arready_i && !rd_in_progress_q;
            2'd1:    s_axil_arready_o = m1_axil_arready_i && !rd_in_progress_q;
            2'd2:    s_axil_arready_o = m2_axil_arready_i && !rd_in_progress_q;
            default: s_axil_arready_o = 1'b0;
        endcase
    end

    // Track which slave we're reading from
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_in_progress_q <= 1'b0;
            rd_slave_active_q <= 2'd0;
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
        case (rd_slave_active_q)
            2'd0: begin
                s_axil_rdata_o  = m0_axil_rdata_i;
                s_axil_rresp_o  = m0_axil_rresp_i;
                s_axil_rvalid_o = m0_axil_rvalid_i;
            end
            2'd1: begin
                s_axil_rdata_o  = m1_axil_rdata_i;
                s_axil_rresp_o  = m1_axil_rresp_i;
                s_axil_rvalid_o = m1_axil_rvalid_i;
            end
            2'd2: begin
                s_axil_rdata_o  = m2_axil_rdata_i;
                s_axil_rresp_o  = m2_axil_rresp_i;
                s_axil_rvalid_o = m2_axil_rvalid_i;
            end
            default: begin
                s_axil_rdata_o  = 32'hDEAD_BEEF;
                s_axil_rresp_o  = 2'b10;  // SLVERR
                s_axil_rvalid_o = 1'b0;
            end
        endcase
    end

    // Read ready routing
    always_comb begin
        m0_axil_rready_o = (rd_slave_active_q == 2'd0) ? s_axil_rready_i : 1'b0;
        m1_axil_rready_o = (rd_slave_active_q == 2'd1) ? s_axil_rready_i : 1'b0;
        m2_axil_rready_o = (rd_slave_active_q == 2'd2) ? s_axil_rready_i : 1'b0;
    end

    // =========================================================================
    // Write Path
    // =========================================================================

    logic [1:0] wr_slave_sel;
    logic [1:0] wr_slave_active_q;
    logic       wr_in_progress_q;

    always_comb begin
        wr_slave_sel = decode_slave(s_axil_awaddr_i);
    end

    // Address routing
    // Note: For dpi_regfile (m1), subtract 0x100 to get relative addresses
    // Note: For scan_ctrl (m2), use low 12 bits
    always_comb begin
        m0_axil_awaddr_o  = s_axil_awaddr_i[7:0];
        m0_axil_awvalid_o = 1'b0;
        m1_axil_awaddr_o  = s_axil_awaddr_i[15:0] - 16'h0100;  // Subtract base
        m1_axil_awvalid_o = 1'b0;
        m2_axil_awaddr_o  = s_axil_awaddr_i[11:0];
        m2_axil_awvalid_o = 1'b0;

        if (s_axil_awvalid_i && !wr_in_progress_q) begin
            case (wr_slave_sel)
                2'd0: m0_axil_awvalid_o = 1'b1;
                2'd1: m1_axil_awvalid_o = 1'b1;
                2'd2: m2_axil_awvalid_o = 1'b1;
                default: ;
            endcase
        end
    end

    // Data routing
    always_comb begin
        m0_axil_wdata_o  = s_axil_wdata_i;
        m0_axil_wvalid_o = 1'b0;
        m1_axil_wdata_o  = s_axil_wdata_i;
        m1_axil_wvalid_o = 1'b0;
        m2_axil_wdata_o  = s_axil_wdata_i;
        m2_axil_wvalid_o = 1'b0;

        if (s_axil_wvalid_i && !wr_in_progress_q) begin
            case (wr_slave_sel)
                2'd0: m0_axil_wvalid_o = 1'b1;
                2'd1: m1_axil_wvalid_o = 1'b1;
                2'd2: m2_axil_wvalid_o = 1'b1;
                default: ;
            endcase
        end
    end

    // Ready mux
    always_comb begin
        case (wr_slave_sel)
            2'd0: begin
                s_axil_awready_o = m0_axil_awready_i && !wr_in_progress_q;
                s_axil_wready_o  = m0_axil_wready_i && !wr_in_progress_q;
            end
            2'd1: begin
                s_axil_awready_o = m1_axil_awready_i && !wr_in_progress_q;
                s_axil_wready_o  = m1_axil_wready_i && !wr_in_progress_q;
            end
            2'd2: begin
                s_axil_awready_o = m2_axil_awready_i && !wr_in_progress_q;
                s_axil_wready_o  = m2_axil_wready_i && !wr_in_progress_q;
            end
            default: begin
                s_axil_awready_o = 1'b0;
                s_axil_wready_o  = 1'b0;
            end
        endcase
    end

    // Track which slave we're writing to
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_in_progress_q  <= 1'b0;
            wr_slave_active_q <= 2'd0;
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
        case (wr_slave_active_q)
            2'd0: begin
                s_axil_bresp_o  = m0_axil_bresp_i;
                s_axil_bvalid_o = m0_axil_bvalid_i;
            end
            2'd1: begin
                s_axil_bresp_o  = m1_axil_bresp_i;
                s_axil_bvalid_o = m1_axil_bvalid_i;
            end
            2'd2: begin
                s_axil_bresp_o  = m2_axil_bresp_i;
                s_axil_bvalid_o = m2_axil_bvalid_i;
            end
            default: begin
                s_axil_bresp_o  = 2'b10;  // SLVERR
                s_axil_bvalid_o = 1'b0;
            end
        endcase
    end

    // Write response ready routing
    always_comb begin
        m0_axil_bready_o = (wr_slave_active_q == 2'd0) ? s_axil_bready_i : 1'b0;
        m1_axil_bready_o = (wr_slave_active_q == 2'd1) ? s_axil_bready_i : 1'b0;
        m2_axil_bready_o = (wr_slave_active_q == 2'd2) ? s_axil_bready_i : 1'b0;
    end

endmodule
