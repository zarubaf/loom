// SPDX-License-Identifier: Apache-2.0
// Copyright lowRISC contributors.
// Copyright Loom contributors.
//
// loom_host_stub - Simulation stub for host-side DPI handling
//
// This module implements DPI function behavior in Verilog for simulation.
// For real hardware, this would be replaced by loom_host_pcie (Xilinx XDMA).

module loom_host_stub #(
  parameter int unsigned FuncIdWidth   = 8,
  parameter int unsigned MaxArgWidth   = 512,
  parameter int unsigned MaxRetWidth   = 64,
  parameter int unsigned HostLatency   = 10,  // Simulated latency in cycles
  parameter int unsigned ScanDataWidth = 64
) (
  input  logic                     clk_i,
  input  logic                     rst_ni,

  // DPI signals from transformed design
  input  logic                     dpi_valid_i,
  input  logic [FuncIdWidth-1:0]   dpi_func_id_i,
  input  logic [MaxArgWidth-1:0]   dpi_args_i,
  output logic [MaxRetWidth-1:0]   dpi_result_o,
  output logic                     dpi_ack_o,

  // Scan chain interface
  output logic                     scan_cmd_valid_o,
  output logic [2:0]               scan_cmd_o,
  output logic [15:0]              scan_shift_count_o,
  output logic [ScanDataWidth-1:0] scan_data_o,
  input  logic [ScanDataWidth-1:0] scan_data_i,
  input  logic                     scan_busy_i,
  input  logic                     scan_done_i
);

  // ---------------------------------------------------------------------------
  // Scan command codes (match loom_scan_ctrl)
  // ---------------------------------------------------------------------------
  localparam logic [2:0] ScanCmdNop     = 3'd0;
  localparam logic [2:0] ScanCmdCapture = 3'd1;
  localparam logic [2:0] ScanCmdRestore = 3'd2;

  // Special DPI function IDs for scan operations (0xF0-0xFF reserved)
  localparam logic [7:0] FuncScanCapture = 8'hF0;
  localparam logic [7:0] FuncScanRestore = 8'hF1;

  // ---------------------------------------------------------------------------
  // State machine
  // ---------------------------------------------------------------------------
  typedef enum logic [1:0] {
    StIdle,
    StProcessing,
    StScanWait,
    StDone
  } state_e;

  state_e state_q, state_d;

  // ---------------------------------------------------------------------------
  // Registers
  // ---------------------------------------------------------------------------
  logic [15:0]            latency_cnt_q, latency_cnt_d;
  logic [MaxRetWidth-1:0] result_q, result_d;
  logic                   is_scan_q, is_scan_d;
  logic [2:0]             scan_cmd_q, scan_cmd_d;
  logic                   scan_cmd_valid_q, scan_cmd_valid_d;
  logic [15:0]            scan_shift_cnt_q, scan_shift_cnt_d;
  logic [ScanDataWidth-1:0] scan_data_q, scan_data_d;

  // Detect scan operations
  wire is_scan_op = (dpi_func_id_i[7:4] == 4'hF);

  // ---------------------------------------------------------------------------
  // Combinational next-state and output logic
  // ---------------------------------------------------------------------------
  always_comb begin
    // Default: hold values
    state_d          = state_q;
    latency_cnt_d    = latency_cnt_q;
    result_d         = result_q;
    is_scan_d        = is_scan_q;
    scan_cmd_d       = scan_cmd_q;
    scan_cmd_valid_d = 1'b0;  // Pulse only
    scan_shift_cnt_d = scan_shift_cnt_q;
    scan_data_d      = scan_data_q;

    // Output defaults
    dpi_ack_o = 1'b0;

    unique case (state_q)
      StIdle: begin
        latency_cnt_d = '0;
        if (dpi_valid_i) begin
          is_scan_d = is_scan_op;

          if (is_scan_op) begin
            // Setup scan command
            unique case (dpi_func_id_i)
              FuncScanCapture: begin
                scan_cmd_d       = ScanCmdCapture;
                scan_shift_cnt_d = dpi_args_i[15:0];
                scan_cmd_valid_d = 1'b1;
              end
              FuncScanRestore: begin
                scan_cmd_d       = ScanCmdRestore;
                scan_shift_cnt_d = dpi_args_i[15:0];
                scan_data_d      = dpi_args_i[16 +: ScanDataWidth];
                scan_cmd_valid_d = 1'b1;
              end
              default: ;
            endcase
            state_d = StScanWait;
          end else begin
            // Compute DPI result
            result_d = compute_dpi_result(dpi_func_id_i, dpi_args_i);
            state_d  = StProcessing;
          end
        end
      end

      StProcessing: begin
        latency_cnt_d = latency_cnt_q + 1'b1;
        if (latency_cnt_q >= HostLatency[15:0]) begin
          state_d = StDone;
        end
      end

      StScanWait: begin
        if (scan_done_i) begin
          result_d = {{(MaxRetWidth-ScanDataWidth){1'b0}}, scan_data_i};
          state_d  = StDone;
        end
      end

      StDone: begin
        dpi_ack_o = 1'b1;
        state_d   = StIdle;
      end

      default: state_d = StIdle;
    endcase
  end

  // ---------------------------------------------------------------------------
  // DPI function computation (simulation model)
  // ---------------------------------------------------------------------------
  function automatic logic [MaxRetWidth-1:0] compute_dpi_result(
    input logic [FuncIdWidth-1:0] func_id,
    input logic [MaxArgWidth-1:0] args
  );
    logic [MaxRetWidth-1:0] result;
    result = '0;

    case (func_id)
      // Function 0: add(a, b) -> a + b
      8'd0: result = args[31:0] + args[63:32];
      // Function 1: placeholder
      8'd1: result = 32'hDEADBEEF;
      default: result = '0;
    endcase

    return result;
  endfunction

  // ---------------------------------------------------------------------------
  // Sequential logic
  // ---------------------------------------------------------------------------
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q          <= StIdle;
      latency_cnt_q    <= '0;
      result_q         <= '0;
      is_scan_q        <= 1'b0;
      scan_cmd_q       <= ScanCmdNop;
      scan_cmd_valid_q <= 1'b0;
      scan_shift_cnt_q <= '0;
      scan_data_q      <= '0;
    end else begin
      state_q          <= state_d;
      latency_cnt_q    <= latency_cnt_d;
      result_q         <= result_d;
      is_scan_q        <= is_scan_d;
      scan_cmd_q       <= scan_cmd_d;
      scan_cmd_valid_q <= scan_cmd_valid_d;
      scan_shift_cnt_q <= scan_shift_cnt_d;
      scan_data_q      <= scan_data_d;
    end
  end

  // ---------------------------------------------------------------------------
  // Output assignments
  // ---------------------------------------------------------------------------
  assign dpi_result_o      = result_q;
  assign scan_cmd_valid_o  = scan_cmd_valid_q;
  assign scan_cmd_o        = scan_cmd_q;
  assign scan_shift_count_o = scan_shift_cnt_q;
  assign scan_data_o       = scan_data_q;

endmodule
