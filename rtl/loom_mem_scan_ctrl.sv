// SPDX-License-Identifier: Apache-2.0
// Copyright Loom contributors.
//
// loom_mem_scan_ctrl - Memory scan controller for state capture
//
// This module controls the memory scanning process for capturing SRAM contents.
// It iterates through memory addresses, reads each word, and serializes the
// data for output.
//
// Operation (Capture):
//   1. Host sends capture command with total bit count
//   2. Controller halts DUT clock, starts memory scanning
//   3. For each memory:
//      a. Iterate through all addresses
//      b. Read word (1 cycle)
//      c. Shift out word serially (width cycles)
//   4. Signal completion when all memories scanned
//
// Parameters describe the memory configuration at synthesis time.
// Currently supports up to 2 memories with different widths/depths.

module loom_mem_scan_ctrl #(
  // Memory 0 configuration
  parameter int unsigned Mem0Depth = 16,
  parameter int unsigned Mem0Width = 8,
  // Memory 1 configuration
  parameter int unsigned Mem1Depth = 8,
  parameter int unsigned Mem1Width = 16,
  // Derived parameters
  localparam int unsigned Mem0AddrWidth = $clog2(Mem0Depth),
  localparam int unsigned Mem1AddrWidth = $clog2(Mem1Depth),
  localparam int unsigned MaxAddrWidth  = (Mem0AddrWidth > Mem1AddrWidth) ? Mem0AddrWidth : Mem1AddrWidth,
  localparam int unsigned MaxDataWidth  = (Mem0Width > Mem1Width) ? Mem0Width : Mem1Width,
  // Total bits to capture: sum of all memory bits
  localparam int unsigned TotalBits = (Mem0Depth * Mem0Width) + (Mem1Depth * Mem1Width)
) (
  input  logic                     clk_i,
  input  logic                     rst_ni,

  // Host command interface
  input  logic                     cmd_valid_i,
  input  logic [1:0]               cmd_i,        // 0=nop, 1=capture
  output logic                     busy_o,
  output logic                     done_o,

  // Memory 0 interface (directly connected to SRAM)
  output logic                     mem0_req_o,
  output logic [Mem0AddrWidth-1:0] mem0_addr_o,
  input  logic [Mem0Width-1:0]     mem0_rdata_i,

  // Memory 1 interface (directly connected to SRAM)
  output logic                     mem1_req_o,
  output logic [Mem1AddrWidth-1:0] mem1_addr_o,
  input  logic [Mem1Width-1:0]     mem1_rdata_i,

  // Serial output
  output logic                     scan_out_o,
  output logic                     scan_valid_o,  // High when scan_out_o is valid

  // Clock gate control
  output logic                     clk_gate_en_o
);

  // ---------------------------------------------------------------------------
  // Command codes
  // ---------------------------------------------------------------------------
  localparam logic [1:0] CmdNop     = 2'd0;
  localparam logic [1:0] CmdCapture = 2'd1;

  // ---------------------------------------------------------------------------
  // State machine
  // ---------------------------------------------------------------------------
  typedef enum logic [2:0] {
    StIdle,       // Waiting for command
    StSetup,      // Setup for memory read
    StRead,       // Wait for memory read
    StShift,      // Shift out word
    StNextAddr,   // Move to next address or memory
    StComplete    // All done
  } state_e;

  state_e state_q, state_d;

  // ---------------------------------------------------------------------------
  // Registers
  // ---------------------------------------------------------------------------
  // Current memory index (0 or 1)
  logic                     mem_idx_q, mem_idx_d;
  // Current address within memory
  logic [MaxAddrWidth-1:0]  addr_q, addr_d;
  // Bit counter for shifting
  logic [5:0]               bit_cnt_q, bit_cnt_d;  // Up to 64 bits per word
  // Shift register for current word
  logic [MaxDataWidth-1:0]  shift_reg_q, shift_reg_d;

  // Current memory's max address and width (based on mem_idx_q)
  // Use MaxAddrWidth+1 to handle depths that are powers of 2
  logic [MaxAddrWidth:0]    cur_max_addr;  // depth - 1
  logic [5:0]               cur_width;

  always_comb begin
    if (mem_idx_q == 1'b0) begin
      cur_max_addr = (MaxAddrWidth+1)'(Mem0Depth - 1);
      cur_width    = 6'(Mem0Width);
    end else begin
      cur_max_addr = (MaxAddrWidth+1)'(Mem1Depth - 1);
      cur_width    = 6'(Mem1Width);
    end
  end

  // ---------------------------------------------------------------------------
  // Combinational next-state and output logic
  // ---------------------------------------------------------------------------
  always_comb begin
    // Default: hold current values
    state_d     = state_q;
    mem_idx_d   = mem_idx_q;
    addr_d      = addr_q;
    bit_cnt_d   = bit_cnt_q;
    shift_reg_d = shift_reg_q;

    // Output defaults
    mem0_req_o    = 1'b0;
    mem0_addr_o   = addr_q[Mem0AddrWidth-1:0];
    mem1_req_o    = 1'b0;
    mem1_addr_o   = addr_q[Mem1AddrWidth-1:0];
    scan_out_o    = 1'b0;
    scan_valid_o  = 1'b0;
    clk_gate_en_o = 1'b1;  // Clock runs by default
    busy_o        = 1'b1;
    done_o        = 1'b0;

    unique case (state_q)
      StIdle: begin
        busy_o = 1'b0;
        if (cmd_valid_i && cmd_i == CmdCapture) begin
          mem_idx_d = 1'b0;
          addr_d    = '0;
          state_d   = StSetup;
        end
      end

      StSetup: begin
        // Halt DUT clock during scan
        clk_gate_en_o = 1'b0;
        // Issue memory read request
        if (mem_idx_q == 1'b0) begin
          mem0_req_o = 1'b1;
        end else begin
          mem1_req_o = 1'b1;
        end
        state_d = StRead;
      end

      StRead: begin
        // Clock halted, wait for memory read data
        clk_gate_en_o = 1'b0;
        // Memory read takes 1 cycle, data available now
        // Load shift register with read data, left-aligned for MSB-first output
        if (mem_idx_q == 1'b0) begin
          // Left-align 8-bit data in 16-bit register
          shift_reg_d = MaxDataWidth'(mem0_rdata_i) << (MaxDataWidth - Mem0Width);
        end else begin
          // Memory 1 width equals MaxDataWidth, no shift needed
          shift_reg_d = MaxDataWidth'(mem1_rdata_i);
        end
        bit_cnt_d = cur_width;
        state_d   = StShift;
      end

      StShift: begin
        // Clock halted, shift out data MSB first
        clk_gate_en_o = 1'b0;
        scan_out_o    = shift_reg_q[MaxDataWidth-1];
        scan_valid_o  = 1'b1;

        if (bit_cnt_q > 1) begin
          // More bits to shift
          bit_cnt_d   = bit_cnt_q - 1'b1;
          shift_reg_d = shift_reg_q << 1;
        end else begin
          // Word done, move to next address
          state_d = StNextAddr;
        end
      end

      StNextAddr: begin
        clk_gate_en_o = 1'b0;

        if (addr_q < cur_max_addr[MaxAddrWidth-1:0]) begin
          // More addresses in current memory
          addr_d  = addr_q + 1'b1;
          state_d = StSetup;
        end else if (mem_idx_q == 1'b0) begin
          // Move to memory 1
          mem_idx_d = 1'b1;
          addr_d    = '0;
          state_d   = StSetup;
        end else begin
          // All memories done
          state_d = StComplete;
        end
      end

      StComplete: begin
        clk_gate_en_o = 1'b0;
        done_o        = 1'b1;
        state_d       = StIdle;
      end

      default: begin
        state_d = StIdle;
      end
    endcase
  end

  // ---------------------------------------------------------------------------
  // Sequential logic
  // ---------------------------------------------------------------------------
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q     <= StIdle;
      mem_idx_q   <= 1'b0;
      addr_q      <= '0;
      bit_cnt_q   <= '0;
      shift_reg_q <= '0;
    end else begin
      state_q     <= state_d;
      mem_idx_q   <= mem_idx_d;
      addr_q      <= addr_d;
      bit_cnt_q   <= bit_cnt_d;
      shift_reg_q <= shift_reg_d;
    end
  end

endmodule
