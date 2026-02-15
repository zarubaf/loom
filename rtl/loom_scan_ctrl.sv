// SPDX-License-Identifier: Apache-2.0
// Copyright lowRISC contributors.
// Copyright Loom contributors.
//
// loom_scan_ctrl - Scan chain controller for state capture/restore
//
// This module controls the scan chain for capturing and restoring flip-flop
// state. It also controls the DUT clock gate to ensure clean scan operations.
//
// Operation:
//   1. Host sends command (capture/restore) with shift count
//   2. Controller halts DUT clock, enables scan mode
//   3. Runs DUT clock for shift_count cycles to shift data
//   4. Halts clock, disables scan mode, signals completion
//
// Commands:
//   - CMD_CAPTURE: Shift out current state (collect scan_out bits)
//   - CMD_RESTORE: Shift in new state (drive scan_in)

module loom_scan_ctrl #(
  parameter int unsigned DataWidth = 64  // Max data transfer width per command
) (
  input  logic                  clk_i,
  input  logic                  rst_ni,

  // Host command interface
  input  logic                  cmd_valid_i,
  input  logic [2:0]            cmd_i,
  input  logic [15:0]           shift_count_i,
  input  logic [DataWidth-1:0]  shift_data_i,
  output logic [DataWidth-1:0]  shift_data_o,
  output logic                  busy_o,
  output logic                  done_o,

  // Scan chain interface
  output logic                  scan_enable_o,
  output logic                  scan_in_o,
  input  logic                  scan_out_i,

  // Clock gate control - active high enables DUT clock
  output logic                  clk_gate_en_o
);

  // ---------------------------------------------------------------------------
  // Command codes
  // ---------------------------------------------------------------------------
  localparam logic [2:0] CmdNop     = 3'd0;
  localparam logic [2:0] CmdCapture = 3'd1;
  localparam logic [2:0] CmdRestore = 3'd2;

  // MSB mask for capture operation - precompute to avoid parameterized
  // operations inside always_comb (iverilog compatibility)
  localparam logic [DataWidth-1:0] MsbMask = {1'b1, {(DataWidth-1){1'b0}}};

  // ---------------------------------------------------------------------------
  // State machine
  // ---------------------------------------------------------------------------
  typedef enum logic [1:0] {
    StIdle,     // Waiting for command, clock halted for scan ops
    StSetup,    // Setup scan signals, clock halted
    StShift,    // Shifting data, clock running
    StComplete  // Operation complete, clock halted
  } state_e;

  state_e state_q, state_d;

  // ---------------------------------------------------------------------------
  // Registers
  // ---------------------------------------------------------------------------
  logic [15:0]           shift_cnt_q, shift_cnt_d;
  logic [DataWidth-1:0]  shift_reg_q, shift_reg_d;
  logic [2:0]            cmd_q, cmd_d;
  logic                  capturing_q, capturing_d;

  // ---------------------------------------------------------------------------
  // Combinational next-state and output logic
  // ---------------------------------------------------------------------------
  always_comb begin
    // Default: hold current values
    state_d     = state_q;
    shift_cnt_d = shift_cnt_q;
    shift_reg_d = shift_reg_q;
    cmd_d       = cmd_q;
    capturing_d = capturing_q;

    // Output defaults
    scan_enable_o = 1'b0;
    scan_in_o     = 1'b0;
    clk_gate_en_o = 1'b1;  // Clock runs by default
    busy_o        = 1'b1;
    done_o        = 1'b0;

    unique case (state_q)
      StIdle: begin
        busy_o = 1'b0;
        if (cmd_valid_i && cmd_i != CmdNop) begin
          // Latch command parameters
          cmd_d       = cmd_i;
          shift_cnt_d = shift_count_i;
          shift_reg_d = shift_data_i;
          capturing_d = (cmd_i == CmdCapture);
          state_d     = StSetup;
        end
      end

      StSetup: begin
        // Halt clock while we set up scan signals
        clk_gate_en_o = 1'b0;
        scan_enable_o = 1'b1;
        // For restore: output MSB (use MsbMask to extract)
        // Data has chain[N-1] at position N-1, chain[0] at position 0
        // We need to output chain[N-1] first (goes into chain[0], ends at chain[N-1])
        scan_in_o     = |(shift_reg_q & MsbMask);
        // After one cycle of setup, start shifting
        state_d = StShift;
      end

      StShift: begin
        // Clock runs only while we have bits to shift
        // Stop clock when shift_cnt_q == 0 to avoid extra DUT clock edge
        clk_gate_en_o = (shift_cnt_q > 0);
        scan_enable_o = 1'b1;
        // For restore: output MSB
        scan_in_o     = |(shift_reg_q & MsbMask);

        if (shift_cnt_q > 0) begin
          // Continue shifting
          shift_cnt_d = shift_cnt_q - 1'b1;

          if (capturing_q) begin
            // Capture: left-shift, put scan_out in LSB
            // Scan chain outputs MSB (chain position N-1) first, so left-shift
            // naturally reverses to get chain[i] at position i
            shift_reg_d = (shift_reg_q << 1) | DataWidth'(scan_out_i);
          end else begin
            // Restore: left-shift (move next bit to MSB for output)
            // Output order: position N-1 (MSB) first, down to position 0
            // This sends chain[N-1] first, which ends up at chain[N-1]
            shift_reg_d = shift_reg_q << 1;
          end
        end else begin
          // Done shifting
          state_d = StComplete;
        end
      end

      StComplete: begin
        // Halt clock, disable scan
        clk_gate_en_o = 1'b0;
        scan_enable_o = 1'b0;
        done_o        = 1'b1;
        state_d       = StIdle;
      end

      default: begin
        state_d = StIdle;
      end
    endcase
  end

  // ---------------------------------------------------------------------------
  // Sequential logic - state register only
  // ---------------------------------------------------------------------------
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q     <= StIdle;
      shift_cnt_q <= '0;
      shift_reg_q <= '0;
      cmd_q       <= CmdNop;
      capturing_q <= 1'b0;
    end else begin
      state_q     <= state_d;
      shift_cnt_q <= shift_cnt_d;
      shift_reg_q <= shift_reg_d;
      cmd_q       <= cmd_d;
      capturing_q <= capturing_d;
    end
  end

  // ---------------------------------------------------------------------------
  // Output assignment
  // ---------------------------------------------------------------------------
  // For capture: data was shifted in from MSB, now in lower bits
  // The first captured bit is at shift_reg_q[DataWidth - shift_count_i]
  // We return the register as-is; host must know the bit ordering
  assign shift_data_o = shift_reg_q;

endmodule
