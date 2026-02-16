// SPDX-License-Identifier: Apache-2.0
// Loom Emulation Wrapper
//
// Wraps a transformed DUT (with loom_dpi_* ports) and provides:
// - Clock gating based on emu_clk_en and DPI stalls
// - Connection between DUT's DPI interface and the DPI regfile
//
// The DUT after dpi_bridge transformation has these ports:
//   - loom_dpi_valid:   DPI call pending (output)
//   - loom_dpi_func_id: Function ID (output, 8 bits)
//   - loom_dpi_args:    Packed arguments (output)
//   - loom_dpi_result:  Return value from host (input)
//
// This wrapper provides clock gating: the DUT's clock is only enabled when
// emu_clk_en_i is high AND no DPI call is pending (or it's been acked).

`timescale 1ns/1ps

module loom_emu_wrapper #(
    parameter int unsigned N_DPI_FUNCS   = 2,
    parameter int unsigned MAX_ARG_WIDTH = 64,
    parameter int unsigned MAX_RET_WIDTH = 64
)(
    input  logic        clk_i,
    input  logic        rst_ni,

    // Clock enable from emu_ctrl
    input  logic        emu_clk_en_i,

    // DPI stall output to emu_ctrl
    output logic        dpi_stall_o,

    // DPI regfile interface
    output logic [N_DPI_FUNCS-1:0]         dpi_call_valid_o,
    input  logic [N_DPI_FUNCS-1:0]         dpi_call_ready_i,
    output logic [N_DPI_FUNCS-1:0][7:0][31:0] dpi_call_args_o,

    input  logic [N_DPI_FUNCS-1:0]         dpi_ret_valid_i,
    output logic [N_DPI_FUNCS-1:0]         dpi_ret_ready_o,
    input  logic [N_DPI_FUNCS-1:0][63:0]   dpi_ret_data_i,

    // DUT interface (directly connects to transformed DUT ports)
    // These signals come from the DUT after dpi_bridge transformation
    input  logic                           dut_dpi_valid_i,
    input  logic [7:0]                     dut_dpi_func_id_i,
    input  logic [MAX_ARG_WIDTH-1:0]       dut_dpi_args_i,
    output logic [MAX_RET_WIDTH-1:0]       dut_dpi_result_o,
    output logic                           dut_dpi_ready_o,  // Indicates DPI call accepted and result ready

    // Gated clock output to DUT
    output logic        dut_clk_o,
    output logic        dut_rst_no
);

    // =========================================================================
    // Clock Gating Logic
    // =========================================================================
    // The DUT clock is gated when:
    // - emu_clk_en_i is low (from emu_ctrl state machine), OR
    // - A DPI call is pending and hasn't been serviced yet
    //
    // The "pending and not serviced" condition is: dut_dpi_valid_i && !dpi_ack
    // where dpi_ack means the host has written the result (dpi_ret_valid)

    logic dpi_pending_q;
    logic dpi_ack;
    logic [7:0] current_func_id_q;

    // DPI call is acked when the return is valid for the active function
    assign dpi_ack = dpi_pending_q && dpi_ret_valid_i[current_func_id_q];

    // Clock enable: run when master enables AND (no DPI call OR call is acked)
    logic dut_clk_en;
    assign dut_clk_en = emu_clk_en_i && (!dpi_pending_q || dpi_ack);

    // Stall output to emu_ctrl
    assign dpi_stall_o = dpi_pending_q && !dpi_ack;

    // Gated clock generation
    // In simulation, we use a simple AND gate
    // In FPGA, this would be a BUFGCE or similar
    assign dut_clk_o = clk_i && dut_clk_en;

    // Reset passthrough
    assign dut_rst_no = rst_ni;

    // =========================================================================
    // DPI Call State Machine
    // =========================================================================

    typedef enum logic [1:0] {
        StIdle,
        StCallPending,
        StWaitAck
    } state_e;

    state_e state_q, state_d;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q           <= StIdle;
            dpi_pending_q     <= 1'b0;
            current_func_id_q <= 8'd0;
        end else begin
            state_q <= state_d;

            case (state_q)
                StIdle: begin
                    // When DUT asserts valid and clock is enabled, capture the call
                    if (dut_dpi_valid_i && emu_clk_en_i) begin
                        dpi_pending_q     <= 1'b1;
                        current_func_id_q <= dut_dpi_func_id_i;
                    end
                end

                StCallPending: begin
                    // Call is pending, wait for regfile to accept
                    if (dpi_call_ready_i[current_func_id_q]) begin
                        // Regfile accepted, now wait for host response
                    end
                end

                StWaitAck: begin
                    // Wait for host to write result
                    if (dpi_ret_valid_i[current_func_id_q]) begin
                        dpi_pending_q <= 1'b0;
                    end
                end

                default: ;
            endcase
        end
    end

    always_comb begin
        state_d = state_q;

        case (state_q)
            StIdle: begin
                if (dut_dpi_valid_i && emu_clk_en_i) begin
                    state_d = StCallPending;
                end
            end

            StCallPending: begin
                if (dpi_call_ready_i[current_func_id_q]) begin
                    state_d = StWaitAck;
                end
            end

            StWaitAck: begin
                if (dpi_ret_valid_i[current_func_id_q]) begin
                    state_d = StIdle;
                end
            end

            default: state_d = StIdle;
        endcase
    end

    // =========================================================================
    // DPI Regfile Interface
    // =========================================================================

    // Call valid: only assert for the active function during pending state
    always_comb begin
        dpi_call_valid_o = '0;
        if (state_q == StCallPending) begin
            dpi_call_valid_o[current_func_id_q] = 1'b1;
        end
    end

    // Pack arguments into the regfile format
    // Each function has up to 8 x 32-bit argument registers
    // Simplified: just extract the first two 32-bit words from MAX_ARG_WIDTH=64
    always_comb begin
        for (int i = 0; i < N_DPI_FUNCS; i++) begin
            // Default all args to zero
            for (int j = 0; j < 8; j++) begin
                dpi_call_args_o[i][j] = 32'd0;
            end

            // Only populate for active function
            if (i == int'(current_func_id_q) && state_q == StCallPending) begin
                // Extract 32-bit chunks from packed args (up to MAX_ARG_WIDTH bits)
                dpi_call_args_o[i][0] = dut_dpi_args_i[31:0];
                dpi_call_args_o[i][1] = dut_dpi_args_i[63:32];
                // Args 2-7 remain zero (extend if MAX_ARG_WIDTH > 64)
            end
        end
    end

    // Return ready: ack when we're waiting and result comes
    always_comb begin
        dpi_ret_ready_o = '0;
        if (state_q == StWaitAck) begin
            dpi_ret_ready_o[current_func_id_q] = 1'b1;
        end
    end

    // Result output to DUT: multiplex from the active function's return data
    assign dut_dpi_result_o = dpi_ret_data_i[current_func_id_q][MAX_RET_WIDTH-1:0];

    // Ready signal to DUT: indicates the DPI call has been serviced and result is valid
    // This goes high when we're in WaitAck state and the return is valid
    assign dut_dpi_ready_o = (state_q == StWaitAck) && dpi_ret_valid_i[current_func_id_q];

endmodule
