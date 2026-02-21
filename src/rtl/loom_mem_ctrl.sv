// SPDX-License-Identifier: Apache-2.0
// Loom Memory Controller
//
// AXI-Lite wrapper for the unified shadow memory interface. Provides
// random-access read/write of memory contents via the shadow ports
// inserted by the mem_shadow Yosys pass.
//
// Register Map (offset from base 0x30000):
//   0x00  MEM_STATUS    R    [0]=busy, [1]=done
//   0x04  MEM_CONTROL   W    Command: 1=read, 2=write, 3=preload_start, 4=preload_next
//   0x08  MEM_ADDR      RW   Target address (global byte addr)
//   0x0C  MEM_LENGTH    R    Total address space bytes (from parameter)
//   0x10  MEM_DATA[0]   RW   Data word 0
//   0x14  MEM_DATA[1]   RW   Data word 1 (for wide memories)
//   ...up to MEM_DATA[N-1] for max_width/32 words
//
// Operations:
//   Write:         Host writes MEM_ADDR + MEM_DATA, issues CMD_WRITE.
//   Read:          Host writes MEM_ADDR, issues CMD_READ. Wait done, read MEM_DATA.
//   Preload start: Host writes MEM_ADDR + MEM_DATA, issues CMD_PRELOAD_START.
//   Preload next:  Host writes MEM_DATA, issues CMD_PRELOAD_NEXT (auto-increments addr).

module loom_mem_ctrl #(
    parameter int unsigned ADDR_BITS   = 12,   // Shadow address width
    parameter int unsigned DATA_BITS   = 32,   // Max memory data width
    parameter int unsigned TOTAL_BYTES = 4096, // Total address space
    parameter int unsigned N_DATA_WORDS = (DATA_BITS + 31) / 32  // Data buffer size
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

    // Unified shadow memory interface
    output logic [ADDR_BITS-1:0]  shadow_addr_o,
    output logic [DATA_BITS-1:0]  shadow_wdata_o,
    input  logic [DATA_BITS-1:0]  shadow_rdata_i,
    output logic                  shadow_wen_o,
    output logic                  shadow_ren_o
);

    // =========================================================================
    // State Machine
    // =========================================================================

    typedef enum logic [2:0] {
        StIdle    = 3'd0,
        StWrite   = 3'd1,  // Assert wen for one cycle
        StRead    = 3'd2,  // Assert ren for one cycle
        StWait    = 3'd3,  // Wait one cycle for BRAM read latency
        StDone    = 3'd4
    } state_e;

    // Command codes
    localparam logic [7:0] CMD_READ          = 8'h01;
    localparam logic [7:0] CMD_WRITE         = 8'h02;
    localparam logic [7:0] CMD_PRELOAD_START = 8'h03;
    localparam logic [7:0] CMD_PRELOAD_NEXT  = 8'h04;

    state_e state_q;
    logic [ADDR_BITS-1:0] addr_q;            // Current shadow address
    logic [ADDR_BITS-1:0] preload_addr_q;    // Auto-increment preload address
    logic [31:0] data_q [N_DATA_WORDS];      // Data buffer
    logic        done_q;
    logic        is_read_q;                  // True if current op is read

    // =========================================================================
    // Shadow Interface Outputs
    // =========================================================================

    assign shadow_addr_o  = addr_q;
    assign shadow_wen_o   = (state_q == StWrite);
    assign shadow_ren_o   = (state_q == StRead);

    // Pack data buffer into shadow wdata
    // For single-word case (most common), this is just data_q[0].
    // For multi-word, each word maps to a 32-bit slice of wdata.
    always_comb begin
        shadow_wdata_o = '0;
        for (int w = 0; w < int'(N_DATA_WORDS); w++) begin
            for (int b = 0; b < 32; b++) begin
                if (w * 32 + b < int'(DATA_BITS))
                    shadow_wdata_o[w * 32 + b] = data_q[w][b];
            end
        end
    end

    // =========================================================================
    // AXI-Lite Write Handshake
    // =========================================================================

    logic        wr_addr_valid_q, wr_data_valid_q;
    logic [11:0] wr_addr_q;
    logic [31:0] wr_data_q;

    logic       wr_fire;
    logic       wr_cmd_read;
    logic       wr_cmd_write;
    logic       wr_cmd_preload_start;
    logic       wr_cmd_preload_next;
    logic       wr_clear_done;
    logic       wr_addr_en;
    logic       wr_data_en;
    logic [9:0] wr_data_word_addr;

    assign wr_fire = wr_addr_valid_q && wr_data_valid_q && !axil_bvalid_o;

    always_comb begin
        wr_cmd_read          = 1'b0;
        wr_cmd_write         = 1'b0;
        wr_cmd_preload_start = 1'b0;
        wr_cmd_preload_next  = 1'b0;
        wr_clear_done        = 1'b0;
        wr_addr_en           = 1'b0;
        wr_data_en           = 1'b0;
        wr_data_word_addr    = '0;

        if (wr_fire) begin
            case (wr_addr_q[11:2])
                10'h000: begin  // MEM_STATUS — write-to-clear done
                    wr_clear_done = wr_data_q[1];
                end
                10'h001: begin  // MEM_CONTROL
                    case (wr_data_q[7:0])
                        CMD_READ:          wr_cmd_read          = 1'b1;
                        CMD_WRITE:         wr_cmd_write         = 1'b1;
                        CMD_PRELOAD_START: wr_cmd_preload_start = 1'b1;
                        CMD_PRELOAD_NEXT:  wr_cmd_preload_next  = 1'b1;
                        default: ;
                    endcase
                end
                10'h002: begin  // MEM_ADDR
                    wr_addr_en = 1'b1;
                end
                default: begin
                    // MEM_DATA registers (offset 0x10 = word address 4)
                    if (wr_addr_q[11:2] >= 10'h004 &&
                        wr_addr_q[11:2] < 10'h004 + N_DATA_WORDS[9:0]) begin
                        wr_data_en       = 1'b1;
                        wr_data_word_addr = wr_addr_q[11:2] - 10'h004;
                    end
                end
            endcase
        end
    end

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

            if (wr_fire) begin
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

    // =========================================================================
    // Main FSM
    // =========================================================================

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q        <= StIdle;
            addr_q         <= '0;
            preload_addr_q <= '0;
            done_q         <= 1'b0;
            is_read_q      <= 1'b0;
            for (int i = 0; i < int'(N_DATA_WORDS); i++) begin
                data_q[i] <= 32'd0;
            end
        end else begin
            case (state_q)
                StIdle: begin
                    // Process commands
                    if (wr_cmd_write) begin
                        state_q   <= StWrite;
                        done_q    <= 1'b0;
                        is_read_q <= 1'b0;
                        // addr_q already set by MEM_ADDR write
                    end else if (wr_cmd_read) begin
                        state_q   <= StRead;
                        done_q    <= 1'b0;
                        is_read_q <= 1'b1;
                    end else if (wr_cmd_preload_start) begin
                        state_q        <= StWrite;
                        done_q         <= 1'b0;
                        is_read_q      <= 1'b0;
                        preload_addr_q <= addr_q;
                    end else if (wr_cmd_preload_next) begin
                        // Auto-increment: advance by one word (4 bytes)
                        preload_addr_q <= preload_addr_q + ADDR_BITS'(4);
                        addr_q         <= preload_addr_q + ADDR_BITS'(4);
                        state_q        <= StWrite;
                        done_q         <= 1'b0;
                        is_read_q      <= 1'b0;
                    end

                    // Register writes while idle
                    if (wr_clear_done) begin
                        done_q <= 1'b0;
                    end
                    if (wr_addr_en) begin
                        addr_q <= ADDR_BITS'(wr_data_q);
                    end
                    if (wr_data_en) begin
                        data_q[wr_data_word_addr] <= wr_data_q;
                    end
                end

                StWrite: begin
                    // Shadow wen asserted this cycle, transition to done
                    state_q <= StDone;
                end

                StRead: begin
                    // Shadow ren asserted this cycle, wait one cycle for read latency
                    state_q <= StWait;
                end

                StWait: begin
                    // Capture read data from shadow rdata
                    // Unpack rdata into data buffer words
                    for (int w = 0; w < int'(N_DATA_WORDS); w++) begin
                        data_q[w] <= 32'd0;
                        for (int b = 0; b < 32; b++) begin
                            if (w * 32 + b < int'(DATA_BITS))
                                data_q[w][b] <= shadow_rdata_i[w * 32 + b];
                        end
                    end
                    state_q <= StDone;
                end

                StDone: begin
                    done_q <= 1'b1;
                    if (wr_clear_done) begin
                        done_q  <= 1'b0;
                        state_q <= StIdle;
                    end
                    if (wr_addr_en) begin
                        addr_q <= ADDR_BITS'(wr_data_q);
                    end
                    if (wr_data_en) begin
                        data_q[wr_data_word_addr] <= wr_data_q;
                    end
                end

                default: state_q <= StIdle;
            endcase
        end
    end

    // =========================================================================
    // AXI-Lite Read Interface
    // =========================================================================

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
                    10'h000: axil_rdata_o <= {30'd0, done_q, (state_q != StIdle && state_q != StDone)};  // MEM_STATUS
                    10'h002: axil_rdata_o <= addr_q;       // MEM_ADDR
                    10'h003: axil_rdata_o <= TOTAL_BYTES;  // MEM_LENGTH
                    default: begin
                        // MEM_DATA registers start at offset 0x10 (word address 4)
                        if (rd_addr_q[11:2] >= 10'h004 &&
                            rd_addr_q[11:2] < 10'h004 + N_DATA_WORDS[9:0]) begin
                            axil_rdata_o <= data_q[rd_addr_q[11:2] - 10'h004];
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

endmodule
