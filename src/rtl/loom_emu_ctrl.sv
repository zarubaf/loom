// SPDX-License-Identifier: Apache-2.0
// Loom Emulation Controller
//
// Controls the emulation state machine (run/stop/reset/snapshot/restore),
// bridges DUT DPI calls to the regfile, and generates the loom_en signal that
// enables DUT flip-flops.
//
// loom_en_o is the single authoritative enable for the DUT:
//   loom_en = emu_running && (!dut_dpi_valid || dpi_ack)
// where emu_running accounts for state (Running) and time compare,
// and the DPI term is the combinational fast path.
// Stepping is implemented in software by setting time_cmp = time + N
// then issuing CMD_START.
//
// Register Map (offset from base 0x0000):
//   0x00  EMU_STATUS       R     Current emulation state
//   0x04  EMU_CONTROL      W     Command register
//   0x08  EMU_CYCLE_LO     R     DUT cycle counter [31:0]
//   0x0C  EMU_CYCLE_HI     R     DUT cycle counter [63:32]
//   0x10  EMU_CLK_DIV      W     Clock divider (0 = full speed)
//   0x14  N_DPI_FUNCS      R     Number of DPI functions
//   0x18  N_MEMORIES       R     Number of shadow-ported memories
//   0x1C  N_SCAN_CHAINS    R     Number of scan chains
//   0x20  TOTAL_SCAN_BITS  R     Total scan chain length
//   0x24  MAX_ARGS         R     Max DPI arguments per function
//   0x28  DESIGN_ID        R     Design CRC32 (version check)
//   0x2C  LOOM_VERSION     R     Toolchain version
//   0x30  IRQ_STATUS       R     Aggregated IRQ status
//   0x34  IRQ_ENABLE       W     Aggregated IRQ enable
//   0x38  EMU_FINISH       RW    Finish request: [0]=req, [15:8]=exit_code
//   0x3C  EMU_TIME_LO      R     DUT time counter [31:0]
//   0x40  EMU_TIME_HI      R     DUT time counter [63:32]
//   0x44  EMU_TIME_CMP_LO  RW    Time compare [31:0]
//   0x48  EMU_TIME_CMP_HI  RW    Time compare [63:32]

`timescale 1ns/1ps

module loom_emu_ctrl #(
    parameter int unsigned N_DPI_FUNCS     = 1,
    parameter int unsigned N_MEMORIES      = 0,
    parameter int unsigned N_SCAN_CHAINS   = 1,
    parameter int unsigned TOTAL_SCAN_BITS = 0,
    parameter int unsigned MAX_ARG_WIDTH   = 64,
    parameter int unsigned MAX_RET_WIDTH   = 32,
    parameter int unsigned MAX_ARGS        = 8,
    parameter logic [31:0] DESIGN_ID       = 32'h0,
    parameter logic [31:0] LOOM_VERSION    = 32'h00_01_00  // 0.1.0
)(
    input  logic        clk_i,
    input  logic        rst_ni,

    // AXI-Lite Slave interface
    input  logic [7:0]  axil_araddr_i,
    input  logic        axil_arvalid_i,
    output logic        axil_arready_o,
    output logic [31:0] axil_rdata_o,
    output logic [1:0]  axil_rresp_o,
    output logic        axil_rvalid_o,
    input  logic        axil_rready_i,

    input  logic [7:0]  axil_awaddr_i,
    input  logic        axil_awvalid_i,
    output logic        axil_awready_o,
    input  logic [31:0] axil_wdata_i,
    input  logic        axil_wvalid_i,
    output logic        axil_wready_o,
    output logic [1:0]  axil_bresp_o,
    output logic        axil_bvalid_o,
    input  logic        axil_bready_i,

    // DUT DPI interface
    input  logic                       dut_dpi_valid_i,
    input  logic [7:0]                 dut_dpi_func_id_i,
    input  logic [MAX_ARG_WIDTH-1:0]   dut_dpi_args_i,
    output logic [MAX_RET_WIDTH-1:0]   dut_dpi_result_o,
    output logic                       dut_dpi_ready_o,

    // DPI regfile interface
    output logic [N_DPI_FUNCS-1:0]             dpi_call_valid_o,
    input  logic [N_DPI_FUNCS-1:0]             dpi_call_ready_i,
    output logic [N_DPI_FUNCS*MAX_ARGS*32-1:0] dpi_call_args_o,
    input  logic [N_DPI_FUNCS-1:0]             dpi_ret_valid_i,
    output logic [N_DPI_FUNCS-1:0]             dpi_ret_ready_o,
    input  logic [N_DPI_FUNCS*(64+MAX_ARGS*32)-1:0] dpi_ret_data_i,

    // DUT finish request
    input  logic        dut_finish_req_i,
    input  logic [7:0]  dut_finish_code_i,

    // DUT FF enable output
    output logic        loom_en_o,

    // Cycle counter output
    output logic [63:0] cycle_count_o,

    // Finish output
    output logic        finish_o,

    // IRQ output
    output logic        irq_state_change_o
);

    // =========================================================================
    // Types
    // =========================================================================

    typedef enum logic [2:0] {
        StIdle      = 3'd0,
        StRunning   = 3'd1,
        StFrozen    = 3'd2,
        StSnapshot  = 3'd3,
        StRestore   = 3'd4,
        StError     = 3'd5
    } emu_state_e;

    typedef enum logic [1:0] {
        StDpiIdle     = 3'd0,
        StDpiForward  = 3'd1,
        StDpiWait     = 3'd2,
        StDpiComplete = 3'd3
    } dpi_state_e;

    localparam logic [7:0] CMD_START    = 8'h01;
    localparam logic [7:0] CMD_STOP     = 8'h02;
    localparam logic [7:0] CMD_RESET    = 8'h03;
    localparam logic [7:0] CMD_SNAPSHOT = 8'h04;
    localparam logic [7:0] CMD_RESTORE  = 8'h05;

    // =========================================================================
    // Signals
    // =========================================================================

    // Emulation state machine
    emu_state_e state_d, state_q;
    logic [63:0] cycle_count_d, cycle_count_q;
    logic [63:0] time_count_d,  time_count_q;
    logic [63:0] time_cmp_d,    time_cmp_q;
    logic [31:0] clk_div_d,     clk_div_q;
    logic [31:0] irq_enable_d,  irq_enable_q;
    logic        state_changed_d, state_changed_q;
    logic [15:0] finish_reg_d,  finish_reg_q;
    logic [7:0]  cmd_reg_d,     cmd_reg_q;
    logic        cmd_valid_d,   cmd_valid_q;

    // DPI call state
    dpi_state_e dpi_state_d, dpi_state_q;
    logic [7:0] dpi_func_id_d, dpi_func_id_q;

    // Derived combinational signals
    logic emu_running;
    logic dpi_ack;

    // AXI-Lite read channel
    logic [7:0]  rd_addr_d,    rd_addr_q;
    logic        rd_pending_d, rd_pending_q;
    logic        arready_d,    arready_q;
    logic        rvalid_d,     rvalid_q;
    logic [31:0] rdata_d,      rdata_q;
    logic [1:0]  rresp_d,      rresp_q;

    // AXI-Lite write channel
    logic [7:0]  wr_addr_d,       wr_addr_q;
    logic [31:0] wr_data_d,       wr_data_q;
    logic        wr_addr_valid_d, wr_addr_valid_q;
    logic        wr_data_valid_d, wr_data_valid_q;
    logic        awready_d,       awready_q;
    logic        wready_d,        wready_q;
    logic        bvalid_d,        bvalid_q;
    logic [1:0]  bresp_d,         bresp_q;

    // Write-side register update requests (from AXI write comb → main comb)
    logic        wr_cmd_valid;
    logic [7:0]  wr_cmd_data;
    logic        wr_clk_div_en;
    logic [31:0] wr_clk_div_data;
    logic        wr_irq_enable_en;
    logic [31:0] wr_irq_enable_data;
    logic        wr_time_cmp_lo_en;
    logic [31:0] wr_time_cmp_lo_data;
    logic        wr_time_cmp_hi_en;
    logic [31:0] wr_time_cmp_hi_data;
    logic        wr_finish_en;
    logic [15:0] wr_finish_data;

    // =========================================================================
    // loom_en: Single Authoritative DUT Enable (combinational)
    // =========================================================================

    assign emu_running = (state_q == StRunning) && (time_count_q < time_cmp_q);

    assign dpi_ack = (dpi_state_q == StDpiComplete);
    assign loom_en_o = emu_running && (!dut_dpi_valid_i || dpi_ack);

    // =========================================================================
    // Emulation State Machine (combinational)
    // =========================================================================

    always_comb begin
        state_d = state_q;

        unique case (state_q)
            StIdle: begin
                if (cmd_valid_q) begin
                    unique case (cmd_reg_q)
                        CMD_START: state_d = StRunning;
                        default:   ;
                    endcase
                end
            end

            StRunning: begin
                if (time_count_q >= time_cmp_q) begin
                    state_d = StFrozen;
                end else if (cmd_valid_q) begin
                    unique case (cmd_reg_q)
                        CMD_STOP:  state_d = StFrozen;
                        CMD_RESET: state_d = StIdle;
                        default:   ;
                    endcase
                end
            end

            StFrozen: begin
                if (cmd_valid_q) begin
                    unique case (cmd_reg_q)
                        CMD_START:    state_d = StRunning;
                        CMD_RESET:    state_d = StIdle;
                        CMD_SNAPSHOT: state_d = StSnapshot;
                        CMD_RESTORE:  state_d = StRestore;
                        default:      ;
                    endcase
                end
            end

            StSnapshot: state_d = StFrozen;
            StRestore:  state_d = StFrozen;

            StError: begin
                if (cmd_valid_q && cmd_reg_q == CMD_RESET) begin
                    state_d = StIdle;
                end
            end

            default: state_d = StIdle;
        endcase
    end

    // =========================================================================
    // DPI Call State Machine (combinational)
    // =========================================================================

    always_comb begin
        dpi_state_d   = dpi_state_q;
        dpi_func_id_d = dpi_func_id_q;

        unique case (dpi_state_q)
            StDpiIdle: begin
                if (dut_dpi_valid_i && emu_running) begin
                    dpi_state_d   = StDpiForward;
                    dpi_func_id_d = dut_dpi_func_id_i;
                end
            end

            StDpiForward: begin
                if (dpi_call_ready_i[dpi_func_id_q]) begin
                    dpi_state_d = StDpiWait;
                end
            end

            StDpiWait: begin
                if (dpi_ret_valid_i[dpi_func_id_q]) begin
                    dpi_state_d = StDpiComplete;
                end
            end

            StDpiComplete: begin
                // Result is registered; release DUT for one cycle then go idle
                dpi_state_d = StDpiIdle;
            end

            default: dpi_state_d = StDpiIdle;
        endcase
    end

    // =========================================================================
    // DPI Regfile Interface (combinational)
    // =========================================================================

    always_comb begin
        dpi_call_valid_o = '0;
        if (dpi_state_q == StDpiForward) begin
            dpi_call_valid_o[dpi_func_id_q] = 1'b1;
        end
    end

    always_comb begin
        dpi_call_args_o = '0;
        if (dpi_state_q == StDpiForward) begin
            for (int w = 0; w < MAX_ARG_WIDTH / 32; w++) begin
                dpi_call_args_o[int'(dpi_func_id_q) * MAX_ARGS * 32 + w * 32 +: 32] =
                    dut_dpi_args_i[w * 32 +: 32];
            end
        end
    end

    always_comb begin
        dpi_ret_ready_o = '0;
        if (dpi_state_q == StDpiWait) begin
            dpi_ret_ready_o[dpi_func_id_q] = 1'b1;
        end
    end

    // Register the DPI result when ret_valid goes high (StDpiWait → StDpiComplete).
    // This ensures the result is stable when dpi_ack releases the DUT one cycle later.
    localparam int RET_DATA_PER_FUNC = 64 + MAX_ARGS * 32;
    logic [MAX_RET_WIDTH-1:0] dpi_result_q;
    always_ff @(posedge clk_i) begin
        if (dpi_state_q == StDpiWait && dpi_ret_valid_i[dpi_func_id_q])
            dpi_result_q <= dpi_ret_data_i[int'(dpi_func_id_q) * RET_DATA_PER_FUNC +: MAX_RET_WIDTH];
    end
    assign dut_dpi_result_o = dpi_result_q;
    assign dut_dpi_ready_o  = dpi_ack;

    // =========================================================================
    // Counters and Control Registers (combinational next-state)
    // =========================================================================

    always_comb begin
        // Defaults: hold current value
        cycle_count_d    = cycle_count_q;
        time_count_d     = time_count_q;
        state_changed_d  = (state_q != state_d);
        cmd_valid_d      = 1'b0;  // one-shot
        cmd_reg_d        = cmd_reg_q;
        finish_reg_d     = finish_reg_q;
        clk_div_d        = clk_div_q;
        irq_enable_d     = irq_enable_q;
        time_cmp_d       = time_cmp_q;

        // Cycle counter
        if (state_q == StIdle) begin
            cycle_count_d = 64'd0;
        end else if (loom_en_o) begin
            cycle_count_d = cycle_count_q + 64'd1;
        end

        // Time counter
        if (state_q == StIdle) begin
            time_count_d = 64'd0;
        end else if (loom_en_o) begin
            time_count_d = time_count_q + 64'd1;
        end

        // DUT-initiated finish (only honor while running — combinational
        // DUT outputs may be undefined before the scan chain initializes FFs)
        if (dut_finish_req_i && !finish_reg_q[0] && state_q == StRunning) begin
            finish_reg_d[0]    = 1'b1;
            finish_reg_d[15:8] = dut_finish_code_i;
        end

        // AXI write-side register updates
        if (wr_cmd_valid) begin
            cmd_reg_d   = wr_cmd_data;
            cmd_valid_d = 1'b1;
        end
        if (wr_clk_div_en)     clk_div_d     = wr_clk_div_data;
        if (wr_irq_enable_en)  irq_enable_d  = wr_irq_enable_data;
        if (wr_time_cmp_lo_en) time_cmp_d[31:0]  = wr_time_cmp_lo_data;
        if (wr_time_cmp_hi_en) time_cmp_d[63:32] = wr_time_cmp_hi_data;
        if (wr_finish_en && !finish_reg_q[0]) begin
            finish_reg_d = wr_finish_data;
        end
    end

    // =========================================================================
    // Main Sequential Process
    // =========================================================================

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q          <= StIdle;
            cycle_count_q    <= 64'd0;
            time_count_q     <= 64'd0;
            time_cmp_q       <= 64'd0;
            clk_div_q        <= 32'd0;
            irq_enable_q     <= 32'd0;
            cmd_reg_q        <= 8'd0;
            cmd_valid_q      <= 1'b0;
            state_changed_q  <= 1'b0;
            finish_reg_q     <= 16'd0;
            dpi_state_q      <= StDpiIdle;
            dpi_func_id_q    <= 8'd0;
        end else begin
            state_q          <= state_d;
            cycle_count_q    <= cycle_count_d;
            time_count_q     <= time_count_d;
            time_cmp_q       <= time_cmp_d;
            clk_div_q        <= clk_div_d;
            irq_enable_q     <= irq_enable_d;
            cmd_reg_q        <= cmd_reg_d;
            cmd_valid_q      <= cmd_valid_d;
            state_changed_q  <= state_changed_d;
            finish_reg_q     <= finish_reg_d;
            dpi_state_q      <= dpi_state_d;
            dpi_func_id_q    <= dpi_func_id_d;
        end
    end

    // =========================================================================
    // Output Assignments
    // =========================================================================

    assign cycle_count_o     = cycle_count_q;
    assign finish_o          = finish_reg_q[0];
    assign irq_state_change_o = state_changed_q && irq_enable_q[2];

    // =========================================================================
    // AXI-Lite Read Channel (combinational)
    // =========================================================================

    always_comb begin
        rd_addr_d    = rd_addr_q;
        rd_pending_d = rd_pending_q;
        arready_d    = 1'b1;
        rvalid_d     = rvalid_q;
        rdata_d      = rdata_q;
        rresp_d      = rresp_q;

        if (axil_arvalid_i && arready_q) begin
            rd_addr_d    = axil_araddr_i;
            rd_pending_d = 1'b1;
        end

        if (rd_pending_q && !rvalid_q) begin
            rvalid_d     = 1'b1;
            rresp_d      = 2'b00;
            rd_pending_d = 1'b0;

            unique case (rd_addr_q[7:2])
                6'h00:   rdata_d = {29'd0, state_q};
                6'h02:   rdata_d = cycle_count_q[31:0];
                6'h03:   rdata_d = cycle_count_q[63:32];
                6'h04:   rdata_d = clk_div_q;
                6'h05:   rdata_d = N_DPI_FUNCS;
                6'h06:   rdata_d = N_MEMORIES;
                6'h07:   rdata_d = N_SCAN_CHAINS;
                6'h08:   rdata_d = TOTAL_SCAN_BITS;
                6'h09:   rdata_d = MAX_ARGS;
                6'h0A:   rdata_d = DESIGN_ID;
                6'h0B:   rdata_d = LOOM_VERSION;
                6'h0C:   rdata_d = {29'd0, state_changed_q, (dpi_state_q != StDpiIdle), 1'b0};
                6'h0D:   rdata_d = irq_enable_q;
                6'h0E:   rdata_d = {16'd0, finish_reg_q};
                6'h0F:   rdata_d = time_count_q[31:0];
                6'h10:   rdata_d = time_count_q[63:32];
                6'h11:   rdata_d = time_cmp_q[31:0];
                6'h12:   rdata_d = time_cmp_q[63:32];
                default: rdata_d = 32'hDEAD_BEEF;
            endcase
        end

        if (rvalid_q && axil_rready_i) begin
            rvalid_d = 1'b0;
        end
    end

    // AXI-Lite Read Channel (sequential)
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_addr_q    <= 8'd0;
            rd_pending_q <= 1'b0;
            arready_q    <= 1'b0;
            rvalid_q     <= 1'b0;
            rdata_q      <= 32'd0;
            rresp_q      <= 2'b00;
        end else begin
            rd_addr_q    <= rd_addr_d;
            rd_pending_q <= rd_pending_d;
            arready_q    <= arready_d;
            rvalid_q     <= rvalid_d;
            rdata_q      <= rdata_d;
            rresp_q      <= rresp_d;
        end
    end

    assign axil_arready_o = arready_q;
    assign axil_rvalid_o  = rvalid_q;
    assign axil_rdata_o   = rdata_q;
    assign axil_rresp_o   = rresp_q;

    // =========================================================================
    // AXI-Lite Write Channel (combinational)
    // =========================================================================

    always_comb begin
        wr_addr_d       = wr_addr_q;
        wr_data_d       = wr_data_q;
        wr_addr_valid_d = wr_addr_valid_q;
        wr_data_valid_d = wr_data_valid_q;
        awready_d       = 1'b1;
        wready_d        = 1'b1;
        bvalid_d        = bvalid_q;
        bresp_d         = bresp_q;

        // Default: no register writes this cycle
        wr_cmd_valid        = 1'b0;
        wr_cmd_data         = 8'd0;
        wr_clk_div_en       = 1'b0;
        wr_clk_div_data     = 32'd0;
        wr_irq_enable_en    = 1'b0;
        wr_irq_enable_data  = 32'd0;
        wr_time_cmp_lo_en   = 1'b0;
        wr_time_cmp_lo_data = 32'd0;
        wr_time_cmp_hi_en   = 1'b0;
        wr_time_cmp_hi_data = 32'd0;
        wr_finish_en        = 1'b0;
        wr_finish_data      = 16'd0;

        if (axil_awvalid_i && awready_q) begin
            wr_addr_d       = axil_awaddr_i;
            wr_addr_valid_d = 1'b1;
        end

        if (axil_wvalid_i && wready_q) begin
            wr_data_d       = axil_wdata_i;
            wr_data_valid_d = 1'b1;
        end

        if (wr_addr_valid_q && wr_data_valid_q && !bvalid_q) begin
            unique case (wr_addr_q[7:2])
                6'h01: begin  // EMU_CONTROL
                    wr_cmd_valid = 1'b1;
                    wr_cmd_data  = wr_data_q[7:0];
                end
                6'h04: begin  // EMU_CLK_DIV
                    wr_clk_div_en   = 1'b1;
                    wr_clk_div_data = wr_data_q;
                end
                6'h0D: begin  // IRQ_ENABLE
                    wr_irq_enable_en   = 1'b1;
                    wr_irq_enable_data = wr_data_q;
                end
                6'h0E: begin  // EMU_FINISH
                    if (wr_data_q[0]) begin
                        wr_finish_en   = 1'b1;
                        wr_finish_data = wr_data_q[15:0];
                    end
                end
                6'h11: begin  // EMU_TIME_CMP_LO
                    wr_time_cmp_lo_en   = 1'b1;
                    wr_time_cmp_lo_data = wr_data_q;
                end
                6'h12: begin  // EMU_TIME_CMP_HI
                    wr_time_cmp_hi_en   = 1'b1;
                    wr_time_cmp_hi_data = wr_data_q;
                end
                default: ;
            endcase

            wr_addr_valid_d = 1'b0;
            wr_data_valid_d = 1'b0;
            bvalid_d        = 1'b1;
            bresp_d         = 2'b00;
        end

        if (bvalid_q && axil_bready_i) begin
            bvalid_d = 1'b0;
        end
    end

    // AXI-Lite Write Channel (sequential)
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_addr_q       <= 8'd0;
            wr_data_q       <= 32'd0;
            wr_addr_valid_q <= 1'b0;
            wr_data_valid_q <= 1'b0;
            awready_q       <= 1'b0;
            wready_q        <= 1'b0;
            bvalid_q        <= 1'b0;
            bresp_q         <= 2'b00;
        end else begin
            wr_addr_q       <= wr_addr_d;
            wr_data_q       <= wr_data_d;
            wr_addr_valid_q <= wr_addr_valid_d;
            wr_data_valid_q <= wr_data_valid_d;
            awready_q       <= awready_d;
            wready_q        <= wready_d;
            bvalid_q        <= bvalid_d;
            bresp_q         <= bresp_d;
        end
    end

    assign axil_awready_o = awready_q;
    assign axil_wready_o  = wready_q;
    assign axil_bvalid_o  = bvalid_q;
    assign axil_bresp_o   = bresp_q;

endmodule
