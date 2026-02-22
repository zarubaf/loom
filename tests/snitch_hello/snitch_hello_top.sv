// SPDX-License-Identifier: Apache-2.0
// Snitch Hello World — Minimal wrapper for Loom FPGA emulation demo
//
// Instantiates a Snitch RV32I core with a 64 KB SRAM (code+data),
// a DPI-based host access port for UART and exit, and no accelerators.
//
// Memory map:
//   0x1000_0000  Host I/O base (read/write via DPI)
//   0x8000_0000  64 KB SRAM (instruction + data, $readmemh preload)

`include "reqrsp_interface/typedef.svh"
`include "snitch_vm/typedef.svh"

// Split read/write DPI: host reads return data that flows into the register
// file, making the design fundamentally non-optimizable (the optimizer cannot
// predict DPI return values).
import "DPI-C" function int dpi_host_read(input int addr);
import "DPI-C" function void dpi_host_write(input int addr, input int wdata, input int strb);

module snitch_hello_top (
    input  logic clk_i,
    input  logic rst_ni
);

    logic [1023:0] useless_flop;
    always_ff @(posedge clk_i, negedge rst_ni) begin : blockName
        if (!rst_ni) begin
            useless_flop <= 1024'hdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef;
        end else begin
            useless_flop <= useless_flop + 1024'h1;
        end
    end

    // =========================================================================
    // Parameters
    // =========================================================================
    localparam int unsigned AddrWidth = 32;
    localparam int unsigned DataWidth = 32;
    localparam int unsigned MemWords  = 16384; // 64 KB / 4
    localparam logic [31:0] BootAddr  = 32'h8000_0000;

    // =========================================================================
    // Type definitions
    // =========================================================================
    typedef logic [AddrWidth-1:0] addr_t;
    typedef logic [DataWidth-1:0] data_t;
    typedef logic [DataWidth/8-1:0] strb_t;

    // ReqRsp types for data port (dreq_t / drsp_t)
    `REQRSP_TYPEDEF_ALL(snitch, addr_t, data_t, strb_t, logic)

    // VM types (pa_t, l0_pte_t) — needed by snitch ports even with VMSupport=0
    `SNITCH_VM_TYPEDEF(AddrWidth)

    // Accelerator request/response (not used, but struct fields must exist)
    typedef struct packed {
        logic [31:0] addr;
        logic [4:0]  id;
        logic [31:0] data_op;
        logic [63:0] data_arga;
        logic [63:0] data_argb;
        logic [63:0] data_argc;
    } acc_req_t;

    typedef struct packed {
        logic [4:0]  id;
        logic        error;
        logic [63:0] data;
    } acc_resp_t;

    // XIF types (EnableXif=0, but x_result_i.we is accessed unconditionally)
    typedef struct packed {
        logic [31:0] instr;
        logic [3:0]  id;
        logic [31:0] hartid;
    } x_issue_req_t;

    typedef struct packed {
        logic accept;
        logic writeback;
        logic dualwrite;
        logic dualread;
        logic loadstore;
        logic ecswrite;
    } x_issue_resp_t;

    typedef struct packed {
        logic [31:0] hartid;
        logic [3:0]  id;
        logic [2:0][31:0] rs;
        logic [2:0]  rs_valid;
    } x_register_t;

    typedef struct packed {
        logic [31:0] hartid;
        logic [3:0]  id;
        logic        commit_kill;
    } x_commit_t;

    typedef struct packed {
        logic [3:0]  id;
        logic [31:0] data;
        logic [4:0]  rd;
        logic        we;
        logic [1:0]  ecsdata;
        logic        ecswe;
        logic        exc;
        logic [5:0]  exccode;
    } x_result_t;

    // =========================================================================
    // Signals
    // =========================================================================
    logic        rst;
    assign rst = ~rst_ni;

    // Instruction port
    addr_t       inst_addr;
    logic [31:0] inst_data;
    logic        inst_valid;
    logic        inst_ready;
    logic        inst_cacheable;
    logic        flush_i_valid;

    // Data port (reqrsp)
    snitch_req_t data_req;
    snitch_rsp_t data_rsp;

    // Accelerator (tied off)
    acc_req_t    acc_qreq;
    logic        acc_qvalid;
    acc_resp_t   acc_prsp;

    // XIF (tied off)
    x_issue_req_t  x_issue_req;
    logic          x_issue_valid;
    x_register_t   x_register;
    logic          x_register_valid;
    x_commit_t     x_commit;
    logic          x_commit_valid;
    logic          x_result_ready;

    // PTW
    logic [1:0]    ptw_valid;
    snitch_pkg::va_t [1:0] ptw_va;
    pa_t  [1:0]    ptw_ppn;

    // FPU
    fpnew_pkg::roundmode_e fpu_rnd_mode;
    fpnew_pkg::fmt_mode_t  fpu_fmt_mode;

    // Misc
    snitch_pkg::core_events_t core_events;
    logic        en_copift;
    logic        barrier;

    // i2f / f2i
    logic [31:0] i2f_rdata;
    logic        i2f_rvalid;

    // =========================================================================
    // SRAM (64 KB, word-addressed)
    // =========================================================================
    logic [31:0] mem [0:MemWords-1];
    initial $readmemh("program.hex", mem);

    // =========================================================================
    // Snitch core
    // =========================================================================
    snitch #(
        .BootAddr       (BootAddr),
        .AddrWidth      (AddrWidth),
        .DataWidth      (DataWidth),
        .RVE            (0),
        .Xdma           (0),
        .Xssr           (0),
        .Xcopift        (0),
        .FP_EN          (0),
        .RVF            (0),
        .RVD            (0),
        .XDivSqrt       (0),
        .XFVEC          (0),
        .XFDOTP         (0),
        .XFAUX          (0),
        .VMSupport      (0),
        .EnableXif      (0),
        .XifIdWidth     (4),
        .DebugSupport   (0),
        .NumIntOutstandingLoads (1),
        .NumIntOutstandingMem   (1),
        .NumDTLBEntries (0),
        .NumITLBEntries (0),
        .CaqDepth       (0),
        .CaqTagWidth    (0),
        .dreq_t         (snitch_req_t),
        .drsp_t         (snitch_rsp_t),
        .acc_req_t      (acc_req_t),
        .acc_resp_t     (acc_resp_t),
        .pa_t           (pa_t),
        .l0_pte_t       (l0_pte_t),
        .x_issue_req_t  (x_issue_req_t),
        .x_issue_resp_t (x_issue_resp_t),
        .x_register_t   (x_register_t),
        .x_commit_t     (x_commit_t),
        .x_result_t     (x_result_t)
    ) i_snitch (
        .clk_i          (clk_i),
        .rst_i          (rst),
        .hart_id_i      (32'd0),
        .irq_i          ('0),
        // Instruction port
        .flush_i_valid_o(flush_i_valid),
        .flush_i_ready_i(1'b1),
        .inst_addr_o    (inst_addr),
        .inst_cacheable_o(inst_cacheable),
        .inst_data_i    (inst_data),
        .inst_valid_o   (inst_valid),
        .inst_ready_i   (inst_ready),
        // Accelerator (tied off)
        .acc_qreq_o     (acc_qreq),
        .acc_qvalid_o   (acc_qvalid),
        .acc_qready_i   (1'b1),
        .acc_prsp_i     (acc_prsp),
        .acc_pvalid_i   (1'b0),
        .acc_pready_o   (),
        // XIF (tied off)
        .x_issue_req_o  (x_issue_req),
        .x_issue_resp_i ('0),
        .x_issue_valid_o(x_issue_valid),
        .x_issue_ready_i(1'b0),
        .x_register_o   (x_register),
        .x_register_valid_o(x_register_valid),
        .x_register_ready_i(1'b0),
        .x_commit_o     (x_commit),
        .x_commit_valid_o(x_commit_valid),
        .x_result_i     ('0),
        .x_result_valid_i(1'b0),
        .x_result_ready_o(x_result_ready),
        // i2f / f2i (tied off)
        .i2f_rdata_o    (i2f_rdata),
        .i2f_rvalid_o   (i2f_rvalid),
        .i2f_rready_i   (1'b0),
        .f2i_wdata_i    (32'd0),
        .f2i_wvalid_i   (1'b0),
        .f2i_wready_o   (),
        // Data port
        .data_req_o     (data_req),
        .data_rsp_i     (data_rsp),
        // PTW (tied off)
        .ptw_valid_o    (ptw_valid),
        .ptw_ready_i    (2'b11),
        .ptw_va_o       (ptw_va),
        .ptw_ppn_o      (ptw_ppn),
        .ptw_pte_i      ('0),
        .ptw_is_4mega_i (2'b00),
        // FPU (tied off)
        .fpu_rnd_mode_o (fpu_rnd_mode),
        .fpu_fmt_mode_o (fpu_fmt_mode),
        .fpu_status_i   ('0),
        // CAQ
        .caq_pvalid_i   (1'b0),
        // Misc
        .core_events_o  (core_events),
        .en_copift_o    (en_copift),
        .barrier_o      (barrier),
        .barrier_i      (1'b0)
    );

    // =========================================================================
    // Instruction port → SRAM (combinational, always ready)
    // =========================================================================
    logic [31:0] inst_addr_word;
    assign inst_addr_word = (inst_addr - BootAddr) >> 2;
    assign inst_ready = 1'b1;
    assign inst_data  = mem[inst_addr_word[$clog2(MemWords)-1:0]];

    // =========================================================================
    // Data port — address routing + SRAM + DPI host I/O
    // =========================================================================
    logic        data_valid;
    logic        data_write;
    logic [31:0] data_addr;
    logic [31:0] data_wdata;
    logic [3:0]  data_strb;
    logic        is_sram;

    assign data_valid = data_req.q_valid;
    assign data_write = data_req.q.write;
    assign data_addr  = data_req.q.addr;
    assign data_wdata = data_req.q.data;
    assign data_strb  = data_req.q.strb;
    assign is_sram    = (data_addr >= BootAddr);

    logic [31:0] sram_word_addr;
    assign sram_word_addr = (data_addr - BootAddr) >> 2;

    // Response register (1-cycle latency)
    logic        data_pvalid_q;
    logic [31:0] data_rdata_q;
    logic [31:0] dpi_rdata_q;

    // SRAM read + DPI host read (registered)
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            data_pvalid_q <= 1'b0;
            data_rdata_q  <= 32'd0;
            dpi_rdata_q   <= 32'd0;
        end else begin
            data_pvalid_q <= data_valid;
            if (data_valid && is_sram) begin
                data_rdata_q <= mem[sram_word_addr[$clog2(MemWords)-1:0]];
            end
            // Host read: DPI return value flows into the register file,
            // preventing the optimizer from proving register contents constant.
            if (data_valid && !is_sram && !data_write) begin
                dpi_rdata_q <= dpi_host_read(data_addr);
            end
        end
    end

    // DPI host write (separate from read path)
    always_ff @(posedge clk_i) begin
        if (data_valid && !is_sram && data_write) begin
            dpi_host_write(data_addr, data_wdata, {28'd0, data_strb});
        end
    end

    // SRAM write (byte-enable)
    always_ff @(posedge clk_i) begin
        if (data_valid && data_write && is_sram) begin
            if (data_strb[0]) mem[sram_word_addr[$clog2(MemWords)-1:0]][7:0]   <= data_wdata[7:0];
            if (data_strb[1]) mem[sram_word_addr[$clog2(MemWords)-1:0]][15:8]  <= data_wdata[15:8];
            if (data_strb[2]) mem[sram_word_addr[$clog2(MemWords)-1:0]][23:16] <= data_wdata[23:16];
            if (data_strb[3]) mem[sram_word_addr[$clog2(MemWords)-1:0]][31:24] <= data_wdata[31:24];
        end
    end

    // Data port response
    logic is_sram_q;
    always_ff @(posedge clk_i) begin
        is_sram_q <= is_sram;
    end

    assign data_rsp.q_ready = 1'b1;
    assign data_rsp.p_valid = data_pvalid_q;
    assign data_rsp.p.data  = is_sram_q ? data_rdata_q : dpi_rdata_q;
    assign data_rsp.p.error = 1'b0;

endmodule
