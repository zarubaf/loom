// SPDX-License-Identifier: Apache-2.0
// Loom Emulation Controller
//
// Controls the emulation state machine (run/stop/step/reset/snapshot/restore)
// and generates the emu_clk_en signal that gates the DUT's clock.
//
// Register Map (offset from base 0x0000):
//   0x00  EMU_STATUS       R     Current emulation state
//   0x04  EMU_CONTROL      W     Command register
//   0x08  EMU_CYCLE_LO     R     DUT cycle counter [31:0]
//   0x0C  EMU_CYCLE_HI     R     DUT cycle counter [63:32]
//   0x10  EMU_STEP_COUNT   W     Number of cycles for STEP command
//   0x14  EMU_CLK_DIV      W     Clock divider (0 = full speed)
//   0x18  DUT_RESET_CTRL   W     Bit 0: assert reset, Bit 1: release reset
//   0x20  N_DPI_FUNCS      R     Number of DPI functions
//   0x24  N_MEMORIES       R     Number of shadow-ported memories
//   0x28  N_SCAN_CHAINS    R     Number of scan chains
//   0x2C  TOTAL_SCAN_BITS  R     Total scan chain length
//   0x34  DESIGN_ID        R     Design CRC32 (version check)
//   0x38  LOOM_VERSION     R     Toolchain version
//   0x40  IRQ_STATUS       R     Aggregated IRQ status
//   0x44  IRQ_ENABLE       W     Aggregated IRQ enable
//   0x4C  EMU_FINISH       RW    Finish request: [0]=req, [15:8]=exit_code

`timescale 1ns/1ps

module loom_emu_ctrl #(
    parameter int unsigned N_DPI_FUNCS     = 1,
    parameter int unsigned N_MEMORIES      = 0,
    parameter int unsigned N_SCAN_CHAINS   = 1,
    parameter int unsigned TOTAL_SCAN_BITS = 0,
    parameter logic [31:0] DESIGN_ID       = 32'h0,
    parameter logic [31:0] LOOM_VERSION    = 32'h00_01_00  // 0.1.0
)(
    input  logic        clk_i,
    input  logic        rst_ni,

    // AXI-Lite Slave interface (directly exposed registers)
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

    // DPI stall inputs (active high = DPI call pending)
    input  logic [N_DPI_FUNCS-1:0] dpi_stall_i,

    // DUT finish request (from transformed $finish/$fatal cells)
    input  logic        dut_finish_req_i,
    input  logic [7:0]  dut_finish_code_i,

    // Clock enable output (to gate DUT clock)
    output logic        emu_clk_en_o,

    // DUT reset output
    output logic        dut_rst_no,

    // Cycle counter output (for debug/status)
    output logic [63:0] cycle_count_o,

    // Finish output (triggers simulation shutdown)
    output logic        finish_o,

    // IRQ output (active high)
    output logic        irq_state_change_o
);

    // =========================================================================
    // State Machine
    // =========================================================================

    typedef enum logic [2:0] {
        StIdle      = 3'd0,
        StRunning   = 3'd1,
        StFrozen    = 3'd2,
        StStepping  = 3'd3,
        StSnapshot  = 3'd4,
        StRestore   = 3'd5,
        StError     = 3'd7
    } state_e;

    // Command definitions
    localparam logic [7:0] CMD_START    = 8'h01;
    localparam logic [7:0] CMD_STOP     = 8'h02;
    localparam logic [7:0] CMD_STEP     = 8'h03;
    localparam logic [7:0] CMD_RESET    = 8'h04;
    localparam logic [7:0] CMD_SNAPSHOT = 8'h05;
    localparam logic [7:0] CMD_RESTORE  = 8'h06;

    state_e state_q, state_d;
    logic [63:0] cycle_count_q;
    logic [31:0] step_count_q;
    logic [31:0] step_remaining_q;
    logic [31:0] clk_div_q;
    logic        dut_reset_q;
    logic [31:0] irq_enable_q;
    logic        state_changed_q;
    logic [15:0] finish_reg_q;  // [0]=finish_req, [15:8]=exit_code

    // DUT reset control signals (set by write logic, processed by state machine)
    logic        dut_reset_assert_req;
    logic        dut_reset_release_req;

    // DPI stall aggregation
    logic dpi_stall_any;
    assign dpi_stall_any = |dpi_stall_i;

    // =========================================================================
    // Clock Enable Logic
    // =========================================================================
    // emu_clk_en is LOW (DUT frozen) when:
    //   - state is FROZEN, SNAPSHOT, RESTORE, IDLE
    //   - state is STEPPING and step_remaining has expired
    //   - any DPI bridge is stalling

    logic emu_clk_en_d;

    always_comb begin
        case (state_q)
            StRunning:  emu_clk_en_d = ~dpi_stall_any;
            StStepping: emu_clk_en_d = (step_remaining_q > 0) && ~dpi_stall_any;
            default:    emu_clk_en_d = 1'b0;  // FROZEN, SNAPSHOT, RESTORE, IDLE, ERROR
        endcase
    end

    assign emu_clk_en_o = emu_clk_en_d;

    // =========================================================================
    // State Machine Logic
    // =========================================================================

    // Command register (directly written, cleared after processing)
    logic [7:0] cmd_reg_q;
    logic       cmd_valid_q;

    always_comb begin
        state_d = state_q;

        case (state_q)
            StIdle: begin
                if (cmd_valid_q) begin
                    case (cmd_reg_q)
                        CMD_START: state_d = StRunning;
                        CMD_STEP:  state_d = StStepping;
                        default:   state_d = StIdle;
                    endcase
                end
            end

            StRunning: begin
                if (cmd_valid_q) begin
                    case (cmd_reg_q)
                        CMD_STOP:  state_d = StFrozen;
                        CMD_RESET: state_d = StIdle;
                        default:   state_d = StRunning;
                    endcase
                end
            end

            StFrozen: begin
                if (cmd_valid_q) begin
                    case (cmd_reg_q)
                        CMD_START:    state_d = StRunning;
                        CMD_STEP:     state_d = StStepping;
                        CMD_RESET:    state_d = StIdle;
                        CMD_SNAPSHOT: state_d = StSnapshot;
                        CMD_RESTORE:  state_d = StRestore;
                        default:      state_d = StFrozen;
                    endcase
                end
            end

            StStepping: begin
                // When step_remaining reaches 0, go to frozen
                if (step_remaining_q == 0 || (step_remaining_q == 1 && emu_clk_en_d)) begin
                    state_d = StFrozen;
                end
                // Can be interrupted by STOP
                if (cmd_valid_q && cmd_reg_q == CMD_STOP) begin
                    state_d = StFrozen;
                end
            end

            StSnapshot: begin
                // For now, immediately complete (actual scan logic is separate)
                state_d = StFrozen;
            end

            StRestore: begin
                // For now, immediately complete (actual scan logic is separate)
                state_d = StFrozen;
            end

            StError: begin
                if (cmd_valid_q && cmd_reg_q == CMD_RESET) begin
                    state_d = StIdle;
                end
            end

            default: state_d = StIdle;
        endcase
    end

    // =========================================================================
    // Sequential Logic
    // =========================================================================

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q          <= StIdle;
            cycle_count_q    <= 64'd0;
            step_count_q     <= 32'd1;
            step_remaining_q <= 32'd0;
            clk_div_q        <= 32'd0;
            dut_reset_q      <= 1'b1;  // DUT in reset initially
            irq_enable_q     <= 32'd0;
            cmd_reg_q        <= 8'd0;
            cmd_valid_q      <= 1'b0;
            state_changed_q  <= 1'b0;
            finish_reg_q     <= 16'd0;
        end else begin
            // State transition
            if (state_q != state_d) begin
                state_changed_q <= 1'b1;
            end else begin
                state_changed_q <= 1'b0;
            end
            state_q <= state_d;

            // Clear command after processing
            cmd_valid_q <= 1'b0;

            // Cycle counter
            if (state_q == StIdle) begin
                cycle_count_q <= 64'd0;
            end else if (emu_clk_en_d) begin
                cycle_count_q <= cycle_count_q + 1;
            end

            // Step counter decrement
            if (state_q == StStepping && emu_clk_en_d && step_remaining_q > 0) begin
                step_remaining_q <= step_remaining_q - 1;
            end

            // Load step count when entering STEPPING state
            if (state_q != StStepping && state_d == StStepping) begin
                step_remaining_q <= step_count_q;
            end

            // DUT reset control - process requests from write logic
            // (Requests are one-shot, set by write logic, cleared here after processing)
            if (dut_reset_assert_req) begin
                dut_reset_q <= 1'b1;
            end else if (dut_reset_release_req) begin
                dut_reset_q <= 1'b0;
            end

            // DUT-initiated finish (from transformed $finish/$fatal cells)
            // Once set, stays set until reset
            if (dut_finish_req_i && !finish_reg_q[0]) begin
                finish_reg_q[0]    <= 1'b1;
                finish_reg_q[15:8] <= dut_finish_code_i;
            end
            // Host-initiated finish is handled in write logic below
        end
    end

    assign cycle_count_o = cycle_count_q;
    assign dut_rst_no = ~dut_reset_q;
    assign finish_o = finish_reg_q[0];
    assign irq_state_change_o = state_changed_q && irq_enable_q[2];

    // =========================================================================
    // AXI-Lite Register Interface
    // =========================================================================

    // Read logic
    logic [7:0] rd_addr_q;
    logic       rd_pending_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_pending_q    <= 1'b0;
            rd_addr_q       <= 8'd0;
            axil_arready_o  <= 1'b0;
            axil_rvalid_o   <= 1'b0;
            axil_rdata_o    <= 32'd0;
            axil_rresp_o    <= 2'b00;
        end else begin
            axil_arready_o <= 1'b1;  // Always ready to accept address

            if (axil_arvalid_i && axil_arready_o) begin
                rd_addr_q    <= axil_araddr_i;
                rd_pending_q <= 1'b1;
            end

            if (rd_pending_q && !axil_rvalid_o) begin
                axil_rvalid_o <= 1'b1;
                axil_rresp_o  <= 2'b00;  // OKAY

                case (rd_addr_q[7:2])
                    6'h00: axil_rdata_o <= {29'd0, state_q};         // EMU_STATUS
                    6'h02: axil_rdata_o <= cycle_count_q[31:0];      // EMU_CYCLE_LO
                    6'h03: axil_rdata_o <= cycle_count_q[63:32];     // EMU_CYCLE_HI
                    6'h04: axil_rdata_o <= step_count_q;             // EMU_STEP_COUNT (readback)
                    6'h05: axil_rdata_o <= clk_div_q;                // EMU_CLK_DIV (readback)
                    6'h08: axil_rdata_o <= N_DPI_FUNCS;              // N_DPI_FUNCS
                    6'h09: axil_rdata_o <= N_MEMORIES;               // N_MEMORIES
                    6'h0A: axil_rdata_o <= N_SCAN_CHAINS;            // N_SCAN_CHAINS
                    6'h0B: axil_rdata_o <= TOTAL_SCAN_BITS;          // TOTAL_SCAN_BITS
                    6'h0D: axil_rdata_o <= DESIGN_ID;                // DESIGN_ID
                    6'h0E: axil_rdata_o <= LOOM_VERSION;             // LOOM_VERSION
                    6'h10: axil_rdata_o <= {29'd0, state_changed_q, dpi_stall_any, 1'b0}; // IRQ_STATUS
                    6'h11: axil_rdata_o <= irq_enable_q;             // IRQ_ENABLE
                    6'h13: axil_rdata_o <= {16'd0, finish_reg_q};    // EMU_FINISH
                    default: axil_rdata_o <= 32'hDEAD_BEEF;
                endcase

                rd_pending_q <= 1'b0;
            end

            if (axil_rvalid_o && axil_rready_i) begin
                axil_rvalid_o <= 1'b0;
            end
        end
    end

    // Write logic
    logic wr_addr_valid_q, wr_data_valid_q;
    logic [7:0] wr_addr_q;
    logic [31:0] wr_data_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_addr_valid_q       <= 1'b0;
            wr_data_valid_q       <= 1'b0;
            wr_addr_q             <= 8'd0;
            wr_data_q             <= 32'd0;
            axil_awready_o        <= 1'b0;
            axil_wready_o         <= 1'b0;
            axil_bvalid_o         <= 1'b0;
            axil_bresp_o          <= 2'b00;
            dut_reset_assert_req  <= 1'b0;
            dut_reset_release_req <= 1'b0;
        end else begin
            axil_awready_o <= 1'b1;  // Always ready
            axil_wready_o  <= 1'b1;

            // Default: clear request signals (one-shot)
            dut_reset_assert_req  <= 1'b0;
            dut_reset_release_req <= 1'b0;

            // Capture address
            if (axil_awvalid_i && axil_awready_o) begin
                wr_addr_q       <= axil_awaddr_i;
                wr_addr_valid_q <= 1'b1;
            end

            // Capture data
            if (axil_wvalid_i && axil_wready_o) begin
                wr_data_q       <= axil_wdata_i;
                wr_data_valid_q <= 1'b1;
            end

            // Process write when both address and data are valid
            if (wr_addr_valid_q && wr_data_valid_q && !axil_bvalid_o) begin
                case (wr_addr_q[7:2])
                    6'h01: begin  // EMU_CONTROL
                        cmd_reg_q   <= wr_data_q[7:0];
                        cmd_valid_q <= 1'b1;
                    end
                    6'h04: step_count_q <= wr_data_q;    // EMU_STEP_COUNT
                    6'h05: clk_div_q    <= wr_data_q;    // EMU_CLK_DIV
                    6'h06: begin  // DUT_RESET_CTRL - use request signals
                        if (wr_data_q[0]) dut_reset_assert_req  <= 1'b1;
                        if (wr_data_q[1]) dut_reset_release_req <= 1'b1;
                    end
                    6'h11: irq_enable_q <= wr_data_q;    // IRQ_ENABLE
                    6'h13: begin  // EMU_FINISH - host-initiated shutdown
                        if (wr_data_q[0] && !finish_reg_q[0]) begin
                            finish_reg_q[0]    <= 1'b1;
                            finish_reg_q[15:8] <= wr_data_q[15:8];
                        end
                    end
                    default: ;
                endcase

                wr_addr_valid_q <= 1'b0;
                wr_data_valid_q <= 1'b0;
                axil_bvalid_o   <= 1'b1;
                axil_bresp_o    <= 2'b00;  // OKAY
            end

            if (axil_bvalid_o && axil_bready_i) begin
                axil_bvalid_o <= 1'b0;
            end
        end
    end

endmodule
