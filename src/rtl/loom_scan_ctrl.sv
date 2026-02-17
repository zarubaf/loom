// SPDX-License-Identifier: Apache-2.0
// Loom Scan Chain Controller
//
// Controls scan chain capture (shift out state) and restore (shift in state).
// During scan operations, this module overrides clock gating to shift data.
//
// Register Map (offset from base):
//   0x00 SCAN_STATUS    R    [0]=busy, [1]=done, [7:4]=error_code
//   0x04 SCAN_CONTROL   W    Command: 1=capture, 2=restore
//   0x08 SCAN_LENGTH    R    Chain length in bits (from parameter)
//   0x0C reserved
//   0x10 SCAN_DATA[0]   RW   First 32 bits of scan data (LSBs)
//   0x14 SCAN_DATA[1]   RW   Next 32 bits
//   ...
//
// Scan data is stored LSB-first: DATA[0][0] is the first bit shifted out/in.
// Maximum supported chain length is 32 * N_DATA_WORDS bits.

`timescale 1ns/1ps

module loom_scan_ctrl #(
    parameter int unsigned CHAIN_LENGTH  = 64,   // Total scan chain bits
    parameter int unsigned N_DATA_WORDS  = (CHAIN_LENGTH + 31) / 32  // Data buffer size
)(
    input  logic        clk_i,
    input  logic        rst_ni,

    // AXI-Lite Slave interface
    input  logic [11:0] axil_araddr_i,
    input  logic        axil_arvalid_i,
    output logic        axil_arready_o,
    output logic [31:0] axil_rdata_o,
    output logic [1:0]  axil_rresp_o,
    output logic        axil_rvalid_o,
    input  logic        axil_rready_i,

    input  logic [11:0] axil_awaddr_i,
    input  logic        axil_awvalid_i,
    output logic        axil_awready_o,
    input  logic [31:0] axil_wdata_i,
    input  logic        axil_wvalid_i,
    output logic        axil_wready_o,
    output logic [1:0]  axil_bresp_o,
    output logic        axil_bvalid_o,
    input  logic        axil_bready_i,

    // Scan chain interface (directly to DUT)
    output logic        scan_enable_o,    // Scan mode enable (to loom_scan_enable)
    output logic        scan_in_o,        // Serial data in (to loom_scan_in)
    input  logic        scan_out_i,       // Serial data out (from loom_scan_out)

    // Clock gate override (OR'd with emu_clk_en in wrapper)
    output logic        scan_clk_en_o,    // Enable clock during scan shift

    // Status output
    output logic        scan_busy_o       // Scan operation in progress
);

    // =========================================================================
    // State Machine
    // =========================================================================

    typedef enum logic [2:0] {
        StIdle     = 3'd0,
        StCapture  = 3'd1,  // Shifting data out of scan chain
        StRestore  = 3'd2,  // Shifting data into scan chain
        StDone     = 3'd3
    } state_e;

    // Command codes
    localparam logic [7:0] CMD_CAPTURE = 8'h01;
    localparam logic [7:0] CMD_RESTORE = 8'h02;

    state_e state_q, state_d;
    logic [$clog2(CHAIN_LENGTH+1)-1:0] shift_count_q;  // Bits remaining to shift
    logic [31:0] scan_data_q [N_DATA_WORDS];           // Scan data buffer
    logic        done_q;                               // Operation completed
    logic [3:0]  error_code_q;

    // =========================================================================
    // Scan Shift Logic
    // =========================================================================

    // Current bit position in the data buffer
    logic [$clog2(CHAIN_LENGTH)-1:0] bit_pos;
    assign bit_pos = CHAIN_LENGTH - shift_count_q;  // 0, 1, 2, ... as we shift

    // Which word and bit within that word
    logic [$clog2(N_DATA_WORDS)-1:0] word_idx;
    logic [4:0] bit_in_word;
    assign word_idx = bit_pos[$clog2(CHAIN_LENGTH)-1:5];
    assign bit_in_word = bit_pos[4:0];

    // Scan input data (for restore): read from buffer
    assign scan_in_o = (state_q == StRestore && shift_count_q > 0) ?
                       scan_data_q[word_idx][bit_in_word] : 1'b0;

    // Scan enable: active during capture or restore
    assign scan_enable_o = (state_q == StCapture || state_q == StRestore);

    // Clock enable for scan: pulse during capture/restore to shift one bit
    // We use a two-phase approach: enable clock for one cycle per bit
    logic shift_pulse_q;
    assign scan_clk_en_o = shift_pulse_q && (state_q == StCapture || state_q == StRestore);

    // Busy output
    assign scan_busy_o = (state_q != StIdle && state_q != StDone);

    // =========================================================================
    // State Machine and Shift Logic
    // =========================================================================

    always_comb begin
        state_d = state_q;

        case (state_q)
            StIdle: begin
                // Wait for command (handled in write logic)
            end

            StCapture, StRestore: begin
                if (shift_count_q == 0) begin
                    state_d = StDone;
                end
            end

            StDone: begin
                // Stay in done until read (handled in sequential logic)
            end

            default: state_d = StIdle;
        endcase
    end

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q       <= StIdle;
            shift_count_q <= '0;
            done_q        <= 1'b0;
            error_code_q  <= 4'd0;
            shift_pulse_q <= 1'b0;
            for (int i = 0; i < N_DATA_WORDS; i++) begin
                scan_data_q[i] <= 32'd0;
            end
        end else begin
            state_q <= state_d;

            case (state_q)
                StCapture: begin
                    // Generate shift pulses
                    if (shift_count_q > 0) begin
                        if (!shift_pulse_q) begin
                            shift_pulse_q <= 1'b1;
                        end else begin
                            // After clock edge, capture the output bit
                            scan_data_q[word_idx][bit_in_word] <= scan_out_i;
                            shift_count_q <= shift_count_q - 1;
                            shift_pulse_q <= 1'b0;
                        end
                    end
                end

                StRestore: begin
                    // Generate shift pulses
                    if (shift_count_q > 0) begin
                        if (!shift_pulse_q) begin
                            shift_pulse_q <= 1'b1;
                        end else begin
                            shift_count_q <= shift_count_q - 1;
                            shift_pulse_q <= 1'b0;
                        end
                    end
                end

                StDone: begin
                    done_q <= 1'b1;
                    shift_pulse_q <= 1'b0;
                end

                default: begin
                    shift_pulse_q <= 1'b0;
                end
            endcase
        end
    end

    // =========================================================================
    // AXI-Lite Register Interface
    // =========================================================================

    // Read logic
    logic [11:0] rd_addr_q;
    logic        rd_pending_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_pending_q   <= 1'b0;
            rd_addr_q      <= 12'd0;
            axil_arready_o <= 1'b0;
            axil_rvalid_o  <= 1'b0;
            axil_rdata_o   <= 32'd0;
            axil_rresp_o   <= 2'b00;
        end else begin
            axil_arready_o <= 1'b1;

            if (axil_arvalid_i && axil_arready_o) begin
                rd_addr_q    <= axil_araddr_i;
                rd_pending_q <= 1'b1;
            end

            if (rd_pending_q && !axil_rvalid_o) begin
                axil_rvalid_o <= 1'b1;
                axil_rresp_o  <= 2'b00;

                case (rd_addr_q[11:2])
                    10'h000: axil_rdata_o <= {24'd0, error_code_q, 2'd0, done_q, scan_busy_o};  // SCAN_STATUS
                    10'h002: axil_rdata_o <= CHAIN_LENGTH;  // SCAN_LENGTH
                    default: begin
                        // SCAN_DATA registers start at offset 0x10 (word address 4)
                        if (rd_addr_q[11:2] >= 10'h004 && rd_addr_q[11:2] < 10'h004 + N_DATA_WORDS) begin
                            axil_rdata_o <= scan_data_q[rd_addr_q[11:2] - 10'h004];
                        end else begin
                            axil_rdata_o <= 32'hDEAD_BEEF;
                        end
                    end
                endcase

                rd_pending_q <= 1'b0;
            end

            if (axil_rvalid_o && axil_rready_i) begin
                axil_rvalid_o <= 1'b0;
            end
        end
    end

    // Write logic
    logic        wr_addr_valid_q, wr_data_valid_q;
    logic [11:0] wr_addr_q;
    logic [31:0] wr_data_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_addr_valid_q <= 1'b0;
            wr_data_valid_q <= 1'b0;
            wr_addr_q       <= 12'd0;
            wr_data_q       <= 32'd0;
            axil_awready_o  <= 1'b0;
            axil_wready_o   <= 1'b0;
            axil_bvalid_o   <= 1'b0;
            axil_bresp_o    <= 2'b00;
        end else begin
            axil_awready_o <= 1'b1;
            axil_wready_o  <= 1'b1;

            if (axil_awvalid_i && axil_awready_o) begin
                wr_addr_q       <= axil_awaddr_i;
                wr_addr_valid_q <= 1'b1;
            end

            if (axil_wvalid_i && axil_wready_o) begin
                wr_data_q       <= axil_wdata_i;
                wr_data_valid_q <= 1'b1;
            end

            if (wr_addr_valid_q && wr_data_valid_q && !axil_bvalid_o) begin
                case (wr_addr_q[11:2])
                    10'h000: begin  // SCAN_STATUS - Write to clear done
                        if (wr_data_q[1]) begin
                            done_q <= 1'b0;
                            if (state_q == StDone) begin
                                state_q <= StIdle;
                            end
                        end
                    end
                    10'h001: begin  // SCAN_CONTROL
                        if (state_q == StIdle) begin
                            case (wr_data_q[7:0])
                                CMD_CAPTURE: begin
                                    state_q       <= StCapture;
                                    shift_count_q <= CHAIN_LENGTH[$clog2(CHAIN_LENGTH+1)-1:0];
                                    done_q        <= 1'b0;
                                    error_code_q  <= 4'd0;
                                end
                                CMD_RESTORE: begin
                                    state_q       <= StRestore;
                                    shift_count_q <= CHAIN_LENGTH[$clog2(CHAIN_LENGTH+1)-1:0];
                                    done_q        <= 1'b0;
                                    error_code_q  <= 4'd0;
                                end
                                default: ;
                            endcase
                        end
                    end
                    default: begin
                        // SCAN_DATA registers
                        if (wr_addr_q[11:2] >= 10'h004 && wr_addr_q[11:2] < 10'h004 + N_DATA_WORDS) begin
                            if (state_q == StIdle || state_q == StDone) begin
                                scan_data_q[wr_addr_q[11:2] - 10'h004] <= wr_data_q;
                            end
                        end
                    end
                endcase

                wr_addr_valid_q <= 1'b0;
                wr_data_valid_q <= 1'b0;
                axil_bvalid_o   <= 1'b1;
                axil_bresp_o    <= 2'b00;
            end

            if (axil_bvalid_o && axil_bready_i) begin
                axil_bvalid_o <= 1'b0;
            end
        end
    end

endmodule
