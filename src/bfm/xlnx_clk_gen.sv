// SPDX-License-Identifier: Apache-2.0
// Behavioral Clock Wizard (xlnx_clk_gen)
//
// Simulation replacement for Xilinx Clocking Wizard IP.
// Generates clk_out1 at 100 MHz (independent of clk_in1).
// AXI-Lite DRP interface accepts all transactions with OKAY, returns 0 for reads.

module xlnx_clk_gen (
    input  wire        clk_in1,
    output wire        clk_out1,
    output reg         locked,

    // AXI-Lite DRP slave
    input  wire        s_axi_aclk,
    input  wire        s_axi_aresetn,

    input  wire [10:0] s_axi_araddr,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    input  wire [10:0] s_axi_awaddr,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready
);

    // =========================================================================
    // Clock Generation (100 MHz, independent of clk_in1)
    // =========================================================================
    reg clk_reg;

    initial begin
        clk_reg = 1'b0;
        forever #5 clk_reg = ~clk_reg;  // 100 MHz
    end

    assign clk_out1 = clk_reg;

    // =========================================================================
    // Locked signal — deasserted for 10 cycles after reset, then asserted
    // =========================================================================
    reg [3:0] lock_cnt;

    initial begin
        locked   = 1'b0;
        lock_cnt = 4'd0;
    end

    always_ff @(posedge clk_reg) begin
        if (lock_cnt < 4'd10) begin
            lock_cnt <= lock_cnt + 4'd1;
            locked   <= 1'b0;
        end else begin
            locked <= 1'b1;
        end
    end

    // =========================================================================
    // AXI-Lite stub — accepts all transactions, returns 0 for reads
    // =========================================================================

    // Read channel
    always_ff @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
        if (!s_axi_aresetn) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
            s_axi_rresp   <= 2'b00;
        end else begin
            s_axi_arready <= 1'b1;

            if (s_axi_arvalid && s_axi_arready && !s_axi_rvalid) begin
                s_axi_rvalid <= 1'b1;
                s_axi_rdata  <= 32'd0;
                s_axi_rresp  <= 2'b00;
            end

            if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

    // Write channel
    reg wr_addr_done, wr_data_done;

    always_ff @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
        if (!s_axi_aresetn) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            s_axi_bresp   <= 2'b00;
            wr_addr_done  <= 1'b0;
            wr_data_done  <= 1'b0;
        end else begin
            s_axi_awready <= 1'b1;
            s_axi_wready  <= 1'b1;

            if (s_axi_awvalid && s_axi_awready) wr_addr_done <= 1'b1;
            if (s_axi_wvalid  && s_axi_wready)  wr_data_done <= 1'b1;

            if (wr_addr_done && wr_data_done && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
                wr_addr_done <= 1'b0;
                wr_data_done <= 1'b0;
            end

            if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

endmodule
