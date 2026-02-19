// SPDX-License-Identifier: Apache-2.0
// Loom DPI Register File
//
// Manages DPI function calls via AXI-Lite registers.
// Each DPI function gets a 64-byte region with status, control, args, and result.
//
// Per-function register layout (64 bytes each):
//   0x00  FUNC_STATUS   R    Bit 0: pending, Bit 1: done, Bit 2: error
//   0x04  FUNC_CONTROL  W    Bit 0: ack (clears pending), Bit 1: set_done
//   0x08  ARG0          R    First argument [31:0]
//   0x0C  ARG1          R    Second argument [31:0]
//   0x10  ARG2          R    Third argument [31:0]
//   0x14  ARG3          R    Fourth argument [31:0]
//   0x18  ARG4          R    Fifth argument [31:0]
//   0x1C  ARG5          R    Sixth argument [31:0]
//   0x20  ARG6          R    Seventh argument [31:0]
//   0x24  ARG7          R    Eighth argument [31:0]
//   0x28  RESULT_LO     W    Return value [31:0]
//   0x2C  RESULT_HI     W    Return value [63:32]
//
// Address decoding:
//   addr[15:6] = function index (up to 1024 functions)
//   addr[5:0]  = register within function

`timescale 1ns/1ps

module loom_dpi_regfile #(
    parameter int unsigned N_DPI_FUNCS = 1,
    parameter int unsigned MAX_ARGS    = 8
)(
    input  logic        clk_i,
    input  logic        rst_ni,

    // AXI-Lite Slave interface
    input  logic [15:0] axil_araddr_i,
    input  logic        axil_arvalid_i,
    output logic        axil_arready_o,
    output logic [31:0] axil_rdata_o,
    output logic [1:0]  axil_rresp_o,
    output logic        axil_rvalid_o,
    input  logic        axil_rready_i,

    input  logic [15:0] axil_awaddr_i,
    input  logic        axil_awvalid_i,
    output logic        axil_awready_o,
    input  logic [31:0] axil_wdata_i,
    input  logic        axil_wvalid_i,
    output logic        axil_wready_o,
    output logic [1:0]  axil_bresp_o,
    output logic        axil_bvalid_o,
    input  logic        axil_bready_i,

    // DPI bridge interface (directly to DUT bridge cells)
    // Call interface: DUT asserts call_valid with args, waits for call_ready
    input  logic [N_DPI_FUNCS-1:0]         dpi_call_valid_i,
    output logic [N_DPI_FUNCS-1:0]         dpi_call_ready_o,
    input  logic [N_DPI_FUNCS-1:0][MAX_ARGS-1:0][31:0] dpi_call_args_i,

    // Return interface: host writes result, asserts ret_valid
    // ret_data includes both scalar result (bits [63:0]) and host-written
    // arg registers (bits [64+MAX_ARGS*32-1:64]) for output open array data.
    output logic [N_DPI_FUNCS-1:0]         dpi_ret_valid_o,
    input  logic [N_DPI_FUNCS-1:0]         dpi_ret_ready_i,
    output logic [N_DPI_FUNCS-1:0][64+MAX_ARGS*32-1:0] dpi_ret_data_o,

    // Stall output (active high = at least one DPI call pending)
    output logic [N_DPI_FUNCS-1:0]         dpi_stall_o
);

    // =========================================================================
    // Per-function state
    // =========================================================================

    typedef struct packed {
        logic        pending;      // Call pending, waiting for host ack
        logic        done;         // Host has written result
        logic        error;        // Error flag
        logic [MAX_ARGS-1:0][31:0] args;  // Captured arguments
        logic [63:0] result;       // Return value from host
    } func_state_t;

    func_state_t func_state_q [N_DPI_FUNCS];
    func_state_t func_state_d [N_DPI_FUNCS];

    // Stall when pending but not yet done
    generate
        for (genvar i = 0; i < N_DPI_FUNCS; i++) begin : gen_stall
            assign dpi_stall_o[i] = func_state_q[i].pending && !func_state_q[i].done;
        end
    endgenerate

    // =========================================================================
    // DPI Call Handling
    // =========================================================================

    // When DUT raises call_valid and we're not already pending, capture args
    generate
        for (genvar i = 0; i < N_DPI_FUNCS; i++) begin : gen_call
            assign dpi_call_ready_o[i] = !func_state_q[i].pending;
        end
    endgenerate

    // Return data and valid
    generate
        for (genvar i = 0; i < N_DPI_FUNCS; i++) begin : gen_ret
            assign dpi_ret_valid_o[i] = func_state_q[i].done;
            // Return data: scalar result in [63:0], host-written args in [64+:]
            assign dpi_ret_data_o[i]  = {func_state_q[i].args, func_state_q[i].result};
        end
    endgenerate

    // =========================================================================
    // State Update Logic
    // =========================================================================

    // Register addresses
    localparam logic [5:0] REG_STATUS     = 6'h00;
    localparam logic [5:0] REG_CONTROL    = 6'h01;
    localparam logic [5:0] REG_ARG0       = 6'h02;
    localparam logic [5:0] REG_ARG1       = 6'h03;
    localparam logic [5:0] REG_ARG2       = 6'h04;
    localparam logic [5:0] REG_ARG3       = 6'h05;
    localparam logic [5:0] REG_ARG4       = 6'h06;
    localparam logic [5:0] REG_ARG5       = 6'h07;
    localparam logic [5:0] REG_ARG6       = 6'h08;
    localparam logic [5:0] REG_ARG7       = 6'h09;
    localparam logic [5:0] REG_RESULT_LO  = 6'h0A;
    localparam logic [5:0] REG_RESULT_HI  = 6'h0B;

    // Write address/data capture
    logic wr_addr_valid_q, wr_data_valid_q;
    logic [15:0] wr_addr_q;
    logic [31:0] wr_data_q;

    // Write processing
    logic [9:0] wr_func_idx;
    logic [5:0] wr_reg_idx;
    assign wr_func_idx = wr_addr_q[15:6];
    assign wr_reg_idx  = wr_addr_q[5:2];  // Word-aligned

    // Host write decode
    logic wr_pending;
    assign wr_pending = wr_addr_valid_q && wr_data_valid_q && !axil_bvalid_o
                        && (wr_func_idx < N_DPI_FUNCS);

    // Combinational next-state (merges DPI call events + host AXI writes)
    always_comb begin
        for (int i = 0; i < N_DPI_FUNCS; i++) begin
            func_state_d[i] = func_state_q[i];

            // DPI call capture: when call_valid and not pending
            if (dpi_call_valid_i[i] && !func_state_q[i].pending) begin
                func_state_d[i].pending = 1'b1;
                func_state_d[i].done    = 1'b0;
                func_state_d[i].args    = dpi_call_args_i[i];
            end

            // Return acknowledged: when ret_valid and ret_ready
            if (func_state_q[i].done && dpi_ret_ready_i[i]) begin
                func_state_d[i].pending = 1'b0;
                func_state_d[i].done    = 1'b0;
            end

            // Host AXI write (highest priority)
            if (wr_pending && wr_func_idx == unsigned'(i)) begin
                case (wr_reg_idx)
                    REG_CONTROL: begin
                        if (wr_data_q[1]) func_state_d[i].done  = 1'b1;
                        if (wr_data_q[2]) func_state_d[i].error = 1'b1;
                    end
                    REG_RESULT_LO: func_state_d[i].result[31:0]  = wr_data_q;
                    REG_RESULT_HI: func_state_d[i].result[63:32] = wr_data_q;
                    REG_ARG0: func_state_d[i].args[0] = wr_data_q;
                    REG_ARG1: func_state_d[i].args[1] = wr_data_q;
                    REG_ARG2: func_state_d[i].args[2] = wr_data_q;
                    REG_ARG3: func_state_d[i].args[3] = wr_data_q;
                    REG_ARG4: func_state_d[i].args[4] = wr_data_q;
                    REG_ARG5: func_state_d[i].args[5] = wr_data_q;
                    REG_ARG6: func_state_d[i].args[6] = wr_data_q;
                    REG_ARG7: func_state_d[i].args[7] = wr_data_q;
                    default: ;
                endcase
            end
        end
    end

    // Sequential state update
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            for (int i = 0; i < N_DPI_FUNCS; i++) begin
                func_state_q[i].pending <= 1'b0;
                func_state_q[i].done    <= 1'b0;
                func_state_q[i].error   <= 1'b0;
                func_state_q[i].args    <= '0;
                func_state_q[i].result  <= 64'd0;
            end
        end else begin
            for (int i = 0; i < N_DPI_FUNCS; i++) begin
                func_state_q[i] <= func_state_d[i];
            end
        end
    end

    // =========================================================================
    // AXI-Lite Read Interface
    // =========================================================================

    logic [15:0] rd_addr_q;
    logic        rd_pending_q;

    logic [9:0]  rd_func_idx;
    logic [5:0]  rd_reg_idx;
    assign rd_func_idx = rd_addr_q[15:6];
    assign rd_reg_idx  = rd_addr_q[5:2];

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rd_pending_q   <= 1'b0;
            rd_addr_q      <= 16'd0;
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

                if (rd_func_idx < N_DPI_FUNCS) begin
                    case (rd_reg_idx)
                        REG_STATUS: axil_rdata_o <= {29'd0,
                                                     func_state_q[rd_func_idx].error,
                                                     func_state_q[rd_func_idx].done,
                                                     func_state_q[rd_func_idx].pending};
                        REG_ARG0:   axil_rdata_o <= func_state_q[rd_func_idx].args[0];
                        REG_ARG1:   axil_rdata_o <= func_state_q[rd_func_idx].args[1];
                        REG_ARG2:   axil_rdata_o <= func_state_q[rd_func_idx].args[2];
                        REG_ARG3:   axil_rdata_o <= func_state_q[rd_func_idx].args[3];
                        REG_ARG4:   axil_rdata_o <= func_state_q[rd_func_idx].args[4];
                        REG_ARG5:   axil_rdata_o <= func_state_q[rd_func_idx].args[5];
                        REG_ARG6:   axil_rdata_o <= func_state_q[rd_func_idx].args[6];
                        REG_ARG7:   axil_rdata_o <= func_state_q[rd_func_idx].args[7];
                        REG_RESULT_LO: axil_rdata_o <= func_state_q[rd_func_idx].result[31:0];
                        REG_RESULT_HI: axil_rdata_o <= func_state_q[rd_func_idx].result[63:32];
                        default:    axil_rdata_o <= 32'hDEAD_BEEF;
                    endcase
                end else begin
                    axil_rdata_o <= 32'hDEAD_BEEF;
                    axil_rresp_o <= 2'b10;  // SLVERR
                end

                rd_pending_q <= 1'b0;
            end

            if (axil_rvalid_o && axil_rready_i) begin
                axil_rvalid_o <= 1'b0;
            end
        end
    end

    // =========================================================================
    // AXI-Lite Write Interface
    // =========================================================================

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_addr_valid_q <= 1'b0;
            wr_data_valid_q <= 1'b0;
            wr_addr_q       <= 16'd0;
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
