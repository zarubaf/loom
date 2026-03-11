// SPDX-License-Identifier: Apache-2.0
// Loom DPI Register File
//
// Manages DPI function calls via AXI-Lite registers.
// Each DPI function gets a region with status, control, args, and result.
// MAX_ARGS is computed from the design's actual DPI argument widths.
//
// Per-function register layout:
//   0x00              FUNC_STATUS   R    Bit 0: pending, Bit 1: done, Bit 2: error
//   0x04              FUNC_CONTROL  W    Bit 1: set_done, Bit 2: set_error
//   0x08..0x08+4*N-4  ARG0..ARGN    R/W  Arguments (N = MAX_ARGS)
//   0x08+4*N          RESULT_LO     W    Return value [31:0]
//   0x08+4*N+4        RESULT_HI     W    Return value [63:32]
//
// Address decoding:
//   addr[15:6] = function index (up to 1024 functions)
//   addr[5:0]  = register within function

module loom_dpi_regfile #(
    parameter int unsigned N_DPI_FUNCS      = 1,
    parameter int unsigned MAX_ARGS         = 8,
    parameter bit          HAS_DPI_FIFO     = 1'b0,
    parameter int unsigned FIFO_ENTRY_WORDS = 4,
    parameter int unsigned FIFO_DEPTH_LOG2  = 10
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
    output logic [N_DPI_FUNCS-1:0]         dpi_stall_o,

    // DPI FIFO write interface (from emu_ctrl, read-only DPI calls)
    input  logic                              fifo_wr_valid_i,
    output logic                              fifo_wr_ready_o,
    input  logic [FIFO_ENTRY_WORDS*32-1:0]    fifo_wr_data_i,
    output logic                              fifo_full_o,
    output logic                              fifo_empty_o,
    output logic                              fifo_threshold_o
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
    localparam logic [5:0] REG_ARG0       = 6'h02;  // ARG0..ARG(MAX_ARGS-1) at consecutive offsets
    localparam logic [5:0] REG_RESULT_LO  = REG_ARG0 + MAX_ARGS[5:0];
    localparam logic [5:0] REG_RESULT_HI  = REG_RESULT_LO + 1;

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

    // FIFO write decode (func_idx == 1022)
    logic wr_fifo_pending;
    assign wr_fifo_pending = wr_addr_valid_q && wr_data_valid_q && !axil_bvalid_o
                             && (wr_func_idx == 10'd1022);

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
                    default: begin
                        // ARG registers: REG_ARG0 + k for k in [0, MAX_ARGS)
                        if (wr_reg_idx >= REG_ARG0 &&
                            wr_reg_idx < REG_ARG0 + MAX_ARGS[5:0])
                            func_state_d[i].args[wr_reg_idx - REG_ARG0] = wr_data_q;
                    end
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
    // DPI FIFO (read-only DPI call buffering)
    // =========================================================================

    // Signals exported from FIFO generate block for use in AXI read decode
    logic [15:0] fifo_rd_level;
    logic [15:0] fifo_rd_threshold;
    logic [31:0] fifo_rd_head_word;

    generate if (HAS_DPI_FIFO) begin : gen_fifo
        localparam int DEPTH = 2**FIFO_DEPTH_LOG2;

        logic [FIFO_ENTRY_WORDS*32-1:0] fifo_mem [0:DEPTH-1];
        logic [FIFO_DEPTH_LOG2:0] wr_ptr_q, rd_ptr_q;
        logic [FIFO_DEPTH_LOG2:0] level;
        logic [FIFO_DEPTH_LOG2:0] threshold_q;

        assign level = wr_ptr_q - rd_ptr_q;
        assign fifo_full_o = (level == DEPTH[FIFO_DEPTH_LOG2:0]);
        assign fifo_empty_o = (level == '0);
        assign fifo_wr_ready_o = !fifo_full_o;
        assign fifo_threshold_o = (level >= threshold_q);

        // Export signals for AXI read decode (avoid hierarchical refs)
        assign fifo_rd_level = level[15:0];
        assign fifo_rd_threshold = threshold_q[15:0];

        // Head entry word read (selected by rd_reg_idx in AXI decode)
        // Unpack head entry into an array of 32-bit words for indexed read
        logic [31:0] head_words [0:FIFO_ENTRY_WORDS-1];
        for (genvar w = 0; w < FIFO_ENTRY_WORDS; w++) begin : gen_head_words
            assign head_words[w] = fifo_mem[rd_ptr_q[FIFO_DEPTH_LOG2-1:0]][w*32 +: 32];
        end
        always_comb begin
            fifo_rd_head_word = 32'h0;
            if (!fifo_empty_o && (rd_reg_idx >= REG_ARG0) &&
                (rd_reg_idx < REG_ARG0 + FIFO_ENTRY_WORDS[5:0])) begin
                fifo_rd_head_word = head_words[rd_reg_idx - REG_ARG0];
            end
        end

        // Write side (from emu_ctrl)
        always_ff @(posedge clk_i or negedge rst_ni) begin
            if (!rst_ni) begin
                wr_ptr_q <= '0;
            end else if (fifo_wr_valid_i && fifo_wr_ready_o) begin
                fifo_mem[wr_ptr_q[FIFO_DEPTH_LOG2-1:0]] <= fifo_wr_data_i;
                wr_ptr_q <= wr_ptr_q + 1;
            end
        end

        // Read side (host pop via AXI write to func_idx=1022, CONTROL, bit0)
        always_ff @(posedge clk_i or negedge rst_ni) begin
            if (!rst_ni) begin
                rd_ptr_q    <= '0;
                threshold_q <= {{FIFO_DEPTH_LOG2{1'b0}}, 1'b1};  // default: 1 (trigger on any entry)
            end else begin
                if (wr_fifo_pending) begin
                    case (wr_reg_idx)
                        REG_CONTROL: begin
                            // bit0 = pop
                            if (wr_data_q[0] && !fifo_empty_o)
                                rd_ptr_q <= rd_ptr_q + 1;
                        end
                        REG_ARG0: begin
                            // Threshold register write
                            threshold_q <= wr_data_q[FIFO_DEPTH_LOG2:0];
                        end
                        default: ;
                    endcase
                end
            end
        end
    end else begin : gen_no_fifo
        assign fifo_wr_ready_o  = 1'b0;
        assign fifo_full_o      = 1'b0;
        assign fifo_empty_o     = 1'b1;
        assign fifo_threshold_o = 1'b0;
        assign fifo_rd_level     = 16'b0;
        assign fifo_rd_threshold = 16'b0;
        assign fifo_rd_head_word = 32'h0;
    end endgenerate

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

                if (rd_func_idx == 10'd1023) begin
                    // Global DPI pending mask — one bit per function
                    axil_rdata_o <= 32'd0;
                    for (int i = 0; i < N_DPI_FUNCS && i < 32; i++)
                        axil_rdata_o[i] <= func_state_q[i].pending & ~func_state_q[i].done;
                end else if (rd_func_idx == 10'd1022 && HAS_DPI_FIFO) begin
                    // FIFO registers at func_idx=1022
                    case (rd_reg_idx)
                        REG_STATUS: begin
                            // {level[15:0], 14'b0, full, empty}
                            axil_rdata_o <= {fifo_rd_level[15:0],
                                             14'b0,
                                             fifo_full_o,
                                             fifo_empty_o};
                        end
                        REG_CONTROL: begin
                            // {entry_words[15:0], threshold[15:0]}
                            axil_rdata_o <= {FIFO_ENTRY_WORDS[15:0],
                                             fifo_rd_threshold[15:0]};
                        end
                        default: begin
                            // REG_ARG0 + k: head entry data words
                            if (rd_reg_idx >= REG_ARG0 &&
                                rd_reg_idx < REG_ARG0 + FIFO_ENTRY_WORDS[5:0] &&
                                !fifo_empty_o) begin
                                axil_rdata_o <= fifo_rd_head_word;
                            end else begin
                                axil_rdata_o <= 32'hDEAD_BEEF;
                            end
                        end
                    endcase
                end else if (rd_func_idx < N_DPI_FUNCS) begin
                    case (rd_reg_idx)
                        REG_STATUS: axil_rdata_o <= {29'd0,
                                                     func_state_q[rd_func_idx].error,
                                                     func_state_q[rd_func_idx].done,
                                                     func_state_q[rd_func_idx].pending};
                        REG_RESULT_LO: axil_rdata_o <= func_state_q[rd_func_idx].result[31:0];
                        REG_RESULT_HI: axil_rdata_o <= func_state_q[rd_func_idx].result[63:32];
                        default: begin
                            if (rd_reg_idx >= REG_ARG0 &&
                                rd_reg_idx < REG_ARG0 + MAX_ARGS[5:0])
                                axil_rdata_o <= func_state_q[rd_func_idx].args[rd_reg_idx - REG_ARG0];
                            else
                                axil_rdata_o <= 32'hDEAD_BEEF;
                        end
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
                // Accept writes to valid function indices, pending mask, or FIFO
                if (wr_func_idx < N_DPI_FUNCS || wr_func_idx == 10'd1022 || wr_func_idx == 10'd1023) begin
                    wr_addr_valid_q <= 1'b0;
                    wr_data_valid_q <= 1'b0;
                    axil_bvalid_o   <= 1'b1;
                    axil_bresp_o    <= 2'b00;
                end else begin
                    wr_addr_valid_q <= 1'b0;
                    wr_data_valid_q <= 1'b0;
                    axil_bvalid_o   <= 1'b1;
                    axil_bresp_o    <= 2'b10;  // SLVERR for invalid addresses
                end
            end

            if (axil_bvalid_o && axil_bready_i) begin
                axil_bvalid_o <= 1'b0;
            end
        end
    end

endmodule
