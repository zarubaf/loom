// SPDX-License-Identifier: Apache-2.0
// Behavioral Clock Wizard (xlnx_clk_gen)
//
// Simulation replacement for Xilinx Clocking Wizard IP.
// Models DRP reconfiguration: host writes CLKFBOUT_MULT (reg 0x200) and
// CLKOUT0_DIVIDE (reg 0x208), triggers reconfiguration via reg 0x25C.
// Output clock frequency = 300 × MULT / DIVIDE MHz.
// Default: MULT=2, DIVIDE=12 → 50 MHz (matches FPGA IP default).

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
    // DRP Register File
    // =========================================================================
    // ClkWizCfg0  (0x200): [15:8] = CLKFBOUT_MULT, [7:0] = DIVCLK_DIVIDE
    // ClkWizCfg2  (0x208): CLKOUT0_DIVIDE
    // ClkWizCfg23 (0x25C): Load/SEN trigger (write to apply)
    // ClkWizStatus(0x04):  bit 0 = locked

    reg [31:0] cfg0_reg;    // CLKFBOUT_MULT | DIVCLK_DIVIDE
    reg [31:0] cfg2_reg;    // CLKOUT0_DIVIDE

    // Active MMCM parameters (applied after SEN trigger)
    reg [31:0] active_mult;
    reg [31:0] active_div;

    // Default: MULT=2, DIV=12 → 300*2/12 = 50 MHz
    initial begin
        cfg0_reg    = {16'd0, 8'd2, 8'd1};   // MULT=2, DIVCLK_DIV=1
        cfg2_reg    = 32'd12;                  // CLKOUT0_DIVIDE=12
        active_mult = 32'd2;
        active_div  = 32'd12;
    end

    // Captured write address for AXI write channel
    reg [10:0] wr_addr;

    // =========================================================================
    // Clock Generation — variable frequency from DRP parameters
    // =========================================================================
    // Output freq = 300 × active_mult / active_div MHz
    // half_period_ps = 1e12 / (2 × freq_hz) = 1e6 / (2 × 300 × mult / div)
    //               = 1e6 × div / (600 × mult)

    reg clk_reg;

    // Reconfig event — triggers clock restart.
    // Set in the AXI (s_axi_aclk) domain, cleared in the clk_reg domain:
    // intentional multi-domain write in this behavioral model.
    /* verilator lint_off MULTIDRIVEN */
    reg reconfig_pending;
    /* verilator lint_on MULTIDRIVEN */
    reg [3:0] lock_cnt;

    initial begin
        clk_reg = 1'b0;
        reconfig_pending = 1'b0;
        lock_cnt = 4'd0;
        locked = 1'b0;
    end

    // Clock generation process: uses #delay based on active parameters
    // verilator lint_off STMTDLY
    always begin : clk_gen_proc
        realtime half_period_ns;
        // 300 MHz input × MULT / DIV = output freq
        // half_period = 1e9 / (2 × freq) = 1e9 / (2 × 300e6 × mult / div)
        //            = 1e3 × div / (600 × mult) ns
        half_period_ns = (1000.0 * $itor(active_div)) / (600.0 * $itor(active_mult));
        #(half_period_ns) clk_reg = ~clk_reg;
    end
    // verilator lint_on STMTDLY

    // In simulation, use s_axi_aclk (= aclk from XDMA BFM) as the output clock.
    // The CDC BFM is a wire passthrough, so emu_clk must be the same clock as
    // aclk to avoid cross-domain timing issues. On FPGA, the real Clocking Wizard
    // IP generates a properly synchronized output clock.
    assign clk_out1 = s_axi_aclk;

    // =========================================================================
    // Lock behavior — deassert for 10 output clk cycles after reconfig
    // =========================================================================
    always_ff @(posedge clk_reg) begin
        if (reconfig_pending) begin
            lock_cnt <= 4'd0;
            locked   <= 1'b0;
            reconfig_pending <= 1'b0;
        end else if (lock_cnt < 4'd10) begin
            lock_cnt <= lock_cnt + 4'd1;
            locked   <= 1'b0;
        end else begin
            locked <= 1'b1;
        end
    end

    // =========================================================================
    // AXI-Lite DRP — Read channel
    // =========================================================================
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
                s_axi_rresp  <= 2'b00;

                // Return register data based on address
                case (s_axi_araddr)
                    11'h004:  s_axi_rdata <= {31'd0, locked};  // Status: locked bit
                    11'h200:  s_axi_rdata <= cfg0_reg;
                    11'h208:  s_axi_rdata <= cfg2_reg;
                    default:  s_axi_rdata <= 32'd0;
                endcase
            end

            if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

    // =========================================================================
    // AXI-Lite DRP — Write channel
    // =========================================================================
    reg wr_addr_done, wr_data_done;
    reg [31:0] wr_data_captured;

    always_ff @(posedge s_axi_aclk or negedge s_axi_aresetn) begin
        if (!s_axi_aresetn) begin
            s_axi_awready    <= 1'b0;
            s_axi_wready     <= 1'b0;
            s_axi_bvalid     <= 1'b0;
            s_axi_bresp      <= 2'b00;
            wr_addr_done     <= 1'b0;
            wr_data_done     <= 1'b0;
            wr_addr          <= 11'd0;
            wr_data_captured <= 32'd0;
        end else begin
            s_axi_awready <= 1'b1;
            s_axi_wready  <= 1'b1;

            if (s_axi_awvalid && s_axi_awready) begin
                wr_addr_done <= 1'b1;
                wr_addr      <= s_axi_awaddr;
            end
            if (s_axi_wvalid && s_axi_wready) begin
                wr_data_done     <= 1'b1;
                wr_data_captured <= s_axi_wdata;
            end

            if (wr_addr_done && wr_data_done && !s_axi_bvalid) begin
                s_axi_bvalid <= 1'b1;
                s_axi_bresp  <= 2'b00;
                wr_addr_done <= 1'b0;
                wr_data_done <= 1'b0;

                // Apply write to register file
                case (wr_addr)
                    11'h200: cfg0_reg <= wr_data_captured;
                    11'h208: cfg2_reg <= wr_data_captured;
                    11'h25C: begin
                        // SEN trigger — apply new configuration
                        active_mult <= {24'd0, cfg0_reg[15:8]};
                        active_div  <= cfg2_reg;
                        reconfig_pending <= 1'b1;
                    end
                    default: ; // ignore other addresses
                endcase
            end

            if (s_axi_bvalid && s_axi_bready) begin
                s_axi_bvalid <= 1'b0;
            end
        end
    end

endmodule
