// SPDX-License-Identifier: Apache-2.0
// Controllable downstream AXI-Lite slave for firewall testbench
//
// Two AXI-Lite interfaces:
//   s_ctrl_*  — Control registers (always-ready register file)
//   s_data_*  — Data port with configurable behavior (stall, drain, unsolicited)
//
// Control Register Map (offset from s_ctrl base):
//   0x00  MODE    RW  0=normal, 1=stall, 2=drain, 3=unsol_rd, 4=unsol_wr
//   0x04  DELAY   RW  Response delay in cycles (normal mode)
//   0x08  RDATA   RW  Read data to return
//   0x0C  PENDING RO  rd_pending + wr_pending count
//   0x10  QUIT    WO  Any write asserts quit_o

module fw_test_slave #(
    parameter int unsigned ADDR_WIDTH = 20
)(
    input  logic                    clk_i,
    input  logic                    rst_ni,

    // =====================================================================
    // Control port (always-ready register file)
    // =====================================================================
    input  logic [ADDR_WIDTH-1:0]   s_ctrl_awaddr,
    input  logic                    s_ctrl_awvalid,
    output logic                    s_ctrl_awready,
    input  logic [31:0]             s_ctrl_wdata,
    input  logic [3:0]              s_ctrl_wstrb,
    input  logic                    s_ctrl_wvalid,
    output logic                    s_ctrl_wready,
    output logic [1:0]              s_ctrl_bresp,
    output logic                    s_ctrl_bvalid,
    input  logic                    s_ctrl_bready,

    input  logic [ADDR_WIDTH-1:0]   s_ctrl_araddr,
    input  logic                    s_ctrl_arvalid,
    output logic                    s_ctrl_arready,
    output logic [31:0]             s_ctrl_rdata,
    output logic [1:0]              s_ctrl_rresp,
    output logic                    s_ctrl_rvalid,
    input  logic                    s_ctrl_rready,

    // =====================================================================
    // Data port (configurable behavior)
    // =====================================================================
    input  logic [ADDR_WIDTH-1:0]   s_data_awaddr,
    input  logic                    s_data_awvalid,
    output logic                    s_data_awready,
    input  logic [31:0]             s_data_wdata,
    input  logic [3:0]              s_data_wstrb,
    input  logic                    s_data_wvalid,
    output logic                    s_data_wready,
    output logic [1:0]              s_data_bresp,
    output logic                    s_data_bvalid,
    input  logic                    s_data_bready,

    input  logic [ADDR_WIDTH-1:0]   s_data_araddr,
    input  logic                    s_data_arvalid,
    output logic                    s_data_arready,
    output logic [31:0]             s_data_rdata,
    output logic [1:0]              s_data_rresp,
    output logic                    s_data_rvalid,
    input  logic                    s_data_rready,

    // =====================================================================
    // Sideband
    // =====================================================================
    output logic                    quit_o
);

    // =====================================================================
    // Control Register Address Map (word index = addr[4:2])
    // =====================================================================
    localparam logic [2:0] REG_MODE    = 3'h0;  // 0x00
    localparam logic [2:0] REG_DELAY   = 3'h1;  // 0x04
    localparam logic [2:0] REG_RDATA   = 3'h2;  // 0x08
    localparam logic [2:0] REG_PENDING = 3'h3;  // 0x0C
    localparam logic [2:0] REG_QUIT    = 3'h4;  // 0x10

    // Mode values
    localparam logic [2:0] MODE_NORMAL   = 3'd0;
    localparam logic [2:0] MODE_STALL    = 3'd1;
    localparam logic [2:0] MODE_DRAIN    = 3'd2;
    localparam logic [2:0] MODE_UNSOL_RD = 3'd3;
    localparam logic [2:0] MODE_UNSOL_WR = 3'd4;

    // =====================================================================
    // Control Registers
    // =====================================================================
    logic [2:0]  mode_q;
    logic [31:0] delay_q;
    logic [31:0] rdata_q;
    logic        quit_q;

    assign quit_o = quit_q;

    // =====================================================================
    // Control Port — Always-Ready Register File
    // =====================================================================

    // Read channel state
    logic [7:0]  ctrl_rd_addr_q;
    logic        ctrl_rd_pending_q;
    logic        ctrl_rvalid_q;
    logic [31:0] ctrl_rdata_q;

    // Write channel state
    logic [7:0]  ctrl_wr_addr_q;
    logic [31:0] ctrl_wr_data_q;
    logic        ctrl_wr_addr_valid_q;
    logic        ctrl_wr_data_valid_q;
    logic        ctrl_bvalid_q;

    assign s_ctrl_arready = 1'b1;
    assign s_ctrl_rdata   = ctrl_rdata_q;
    assign s_ctrl_rresp   = 2'b00;
    assign s_ctrl_rvalid  = ctrl_rvalid_q;

    assign s_ctrl_awready = 1'b1;
    assign s_ctrl_wready  = 1'b1;
    assign s_ctrl_bresp   = 2'b00;
    assign s_ctrl_bvalid  = ctrl_bvalid_q;

    // =====================================================================
    // Data Port State
    // =====================================================================
    logic [7:0] rd_pending_q;
    logic [7:0] wr_pending_q;

    // Normal mode delay counter
    logic [31:0] rd_delay_cnt_q;
    logic [31:0] wr_delay_cnt_q;
    logic        rd_responding_q;
    logic        wr_responding_q;

    // Data port read channel
    logic        data_rvalid_q;
    logic [31:0] data_rdata_q;

    // Data port write channels
    logic        data_awready_q;
    logic        data_wready_q;
    logic        data_bvalid_q;

    // =====================================================================
    // Control Port Sequential Logic
    // =====================================================================
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            mode_q  <= MODE_NORMAL;
            delay_q <= 32'd0;
            rdata_q <= 32'hCAFEBABE;
            quit_q  <= 1'b0;

            ctrl_rd_addr_q       <= 8'd0;
            ctrl_rd_pending_q    <= 1'b0;
            ctrl_rvalid_q        <= 1'b0;
            ctrl_rdata_q         <= 32'd0;
            ctrl_wr_addr_q       <= 8'd0;
            ctrl_wr_data_q       <= 32'd0;
            ctrl_wr_addr_valid_q <= 1'b0;
            ctrl_wr_data_valid_q <= 1'b0;
            ctrl_bvalid_q        <= 1'b0;
        end else begin
            // =============================================================
            // Read path
            // =============================================================
            if (s_ctrl_arvalid && s_ctrl_arready) begin
                ctrl_rd_addr_q    <= s_ctrl_araddr[7:0];
                ctrl_rd_pending_q <= 1'b1;
            end

            if (ctrl_rd_pending_q && !ctrl_rvalid_q) begin
                ctrl_rvalid_q     <= 1'b1;
                ctrl_rd_pending_q <= 1'b0;

                case (ctrl_rd_addr_q[4:2])
                    REG_MODE:    ctrl_rdata_q <= {29'd0, mode_q};
                    REG_DELAY:   ctrl_rdata_q <= delay_q;
                    REG_RDATA:   ctrl_rdata_q <= rdata_q;
                    REG_PENDING: ctrl_rdata_q <= {16'd0, wr_pending_q, rd_pending_q};
                    default:     ctrl_rdata_q <= 32'hDEADBEEF;
                endcase
            end

            if (ctrl_rvalid_q && s_ctrl_rready) begin
                ctrl_rvalid_q <= 1'b0;
            end

            // =============================================================
            // Write path
            // =============================================================
            if (s_ctrl_awvalid && s_ctrl_awready) begin
                ctrl_wr_addr_q       <= s_ctrl_awaddr[7:0];
                ctrl_wr_addr_valid_q <= 1'b1;
            end

            if (s_ctrl_wvalid && s_ctrl_wready) begin
                ctrl_wr_data_q       <= s_ctrl_wdata;
                ctrl_wr_data_valid_q <= 1'b1;
            end

            if (ctrl_wr_addr_valid_q && ctrl_wr_data_valid_q && !ctrl_bvalid_q) begin
                ctrl_wr_addr_valid_q <= 1'b0;
                ctrl_wr_data_valid_q <= 1'b0;
                ctrl_bvalid_q        <= 1'b1;

                case (ctrl_wr_addr_q[4:2])
                    REG_MODE:  mode_q  <= ctrl_wr_data_q[2:0];
                    REG_DELAY: delay_q <= ctrl_wr_data_q;
                    REG_RDATA: rdata_q <= ctrl_wr_data_q;
                    REG_QUIT:  quit_q  <= 1'b1;
                    default: ;
                endcase
            end

            if (ctrl_bvalid_q && s_ctrl_bready) begin
                ctrl_bvalid_q <= 1'b0;
            end
        end
    end

    // =====================================================================
    // Data Port Logic
    // =====================================================================

    // Accept requests in NORMAL and STALL modes
    logic accept_rd, accept_wr;
    assign accept_rd = (mode_q == MODE_NORMAL && !rd_responding_q) || (mode_q == MODE_STALL);
    assign accept_wr = (mode_q == MODE_NORMAL && !wr_responding_q) || (mode_q == MODE_STALL);

    assign s_data_arready = accept_rd;
    assign s_data_awready = accept_wr;
    assign s_data_wready  = accept_wr;

    assign s_data_rdata  = data_rdata_q;
    assign s_data_rresp  = 2'b00;
    assign s_data_rvalid = data_rvalid_q;
    assign s_data_bresp  = 2'b00;
    assign s_data_bvalid = data_bvalid_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_pending_q    <= 8'd0;
            wr_pending_q    <= 8'd0;
            rd_delay_cnt_q  <= 32'd0;
            wr_delay_cnt_q  <= 32'd0;
            rd_responding_q <= 1'b0;
            wr_responding_q <= 1'b0;
            data_rvalid_q   <= 1'b0;
            data_rdata_q    <= 32'd0;
            data_bvalid_q   <= 1'b0;
        end else begin
            // Clear valid signals when handshake completes
            if (data_rvalid_q && s_data_rready) begin
                data_rvalid_q <= 1'b0;
            end
            if (data_bvalid_q && s_data_bready) begin
                data_bvalid_q <= 1'b0;
            end

            case (mode_q)
                // ---------------------------------------------------------
                // NORMAL: Accept one request, respond after delay cycles
                // ---------------------------------------------------------
                MODE_NORMAL: begin
                    // Read path
                    if (s_data_arvalid && s_data_arready && !rd_responding_q) begin
                        rd_responding_q <= 1'b1;
                        rd_delay_cnt_q  <= delay_q;
                    end
                    if (rd_responding_q && !data_rvalid_q) begin
                        if (rd_delay_cnt_q == 32'd0) begin
                            data_rvalid_q   <= 1'b1;
                            data_rdata_q    <= rdata_q;
                            rd_responding_q <= 1'b0;
                        end else begin
                            rd_delay_cnt_q <= rd_delay_cnt_q - 32'd1;
                        end
                    end

                    // Write path
                    if (s_data_awvalid && s_data_awready && !wr_responding_q) begin
                        wr_responding_q <= 1'b1;
                        wr_delay_cnt_q  <= delay_q;
                    end
                    if (wr_responding_q && !data_bvalid_q) begin
                        if (wr_delay_cnt_q == 32'd0) begin
                            data_bvalid_q   <= 1'b1;
                            wr_responding_q <= 1'b0;
                        end else begin
                            wr_delay_cnt_q <= wr_delay_cnt_q - 32'd1;
                        end
                    end
                end

                // ---------------------------------------------------------
                // STALL: Accept requests, never generate responses
                // ---------------------------------------------------------
                MODE_STALL: begin
                    if (s_data_arvalid && s_data_arready) begin
                        rd_pending_q <= rd_pending_q + 8'd1;
                    end
                    if (s_data_awvalid && s_data_awready) begin
                        wr_pending_q <= wr_pending_q + 8'd1;
                    end
                end

                // ---------------------------------------------------------
                // DRAIN: Drain queued responses (writes first, then reads)
                //        1 response per cycle, auto-transition to NORMAL
                // ---------------------------------------------------------
                MODE_DRAIN: begin
                    if (wr_pending_q > 8'd0 && !data_bvalid_q) begin
                        data_bvalid_q <= 1'b1;
                        wr_pending_q  <= wr_pending_q - 8'd1;
                    end else if (wr_pending_q == 8'd0 && rd_pending_q > 8'd0 && !data_rvalid_q) begin
                        data_rvalid_q <= 1'b1;
                        data_rdata_q  <= rdata_q;
                        rd_pending_q  <= rd_pending_q - 8'd1;
                    end else if (wr_pending_q == 8'd0 && rd_pending_q == 8'd0) begin
                        mode_q <= MODE_NORMAL;
                    end
                end

                // ---------------------------------------------------------
                // UNSOL_RD: Generate one unsolicited RVALID pulse, then → NORMAL
                // ---------------------------------------------------------
                MODE_UNSOL_RD: begin
                    if (!data_rvalid_q) begin
                        data_rvalid_q <= 1'b1;
                        data_rdata_q  <= rdata_q;
                        mode_q        <= MODE_NORMAL;
                    end
                end

                // ---------------------------------------------------------
                // UNSOL_WR: Generate one unsolicited BVALID pulse, then → NORMAL
                // ---------------------------------------------------------
                MODE_UNSOL_WR: begin
                    if (!data_bvalid_q) begin
                        data_bvalid_q <= 1'b1;
                        mode_q        <= MODE_NORMAL;
                    end
                end

                default: ;
            endcase
        end
    end

endmodule
