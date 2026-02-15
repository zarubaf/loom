// SPDX-License-Identifier: Apache-2.0
// Copyright Loom contributors.
//
// mem_dut.sv - Test DUT with two SRAMs and flip-flops
//
// This module demonstrates a design with:
// - Two SRAMs of different sizes (for memory scan testing)
// - Flip-flops (for FF scan chain testing)
// - Simple accumulator logic that uses both memories

module mem_dut (
  input  logic        clk_i,
  input  logic        rst_ni,

  // Control interface
  input  logic        start_i,       // Start accumulation
  output logic        busy_o,        // Operation in progress
  output logic        done_o,        // Operation complete

  // Memory A: 8-bit x 16 deep (coefficient storage)
  input  logic        mem_a_we_i,
  input  logic [3:0]  mem_a_addr_i,
  input  logic [7:0]  mem_a_wdata_i,
  output logic [7:0]  mem_a_rdata_o,

  // Memory B: 16-bit x 8 deep (result storage)
  input  logic        mem_b_we_i,
  input  logic [2:0]  mem_b_addr_i,
  input  logic [15:0] mem_b_wdata_i,
  output logic [15:0] mem_b_rdata_o,

  // Result output
  output logic [15:0] accumulator_o,
  output logic [3:0]  iteration_o
);

  // ---------------------------------------------------------------------------
  // Internal signals
  // ---------------------------------------------------------------------------

  // State machine
  typedef enum logic [1:0] {
    StIdle,
    StRead,
    StAccum,
    StDone
  } state_e;

  state_e state_q, state_d;

  // Iteration counter (which coefficient we're processing)
  logic [3:0] iter_q, iter_d;

  // Accumulator register
  logic [15:0] accum_q, accum_d;

  // Pipeline register for memory read
  logic [7:0] coeff_q, coeff_d;

  // Internal memory interface signals
  logic        mem_a_req;
  logic [3:0]  mem_a_addr;
  logic [7:0]  mem_a_rdata;

  logic        mem_b_req;
  logic        mem_b_we;
  logic [2:0]  mem_b_addr;
  logic [15:0] mem_b_wdata;
  logic [15:0] mem_b_rdata;

  // ---------------------------------------------------------------------------
  // Memory A: 8-bit x 16 deep (coefficients)
  // ---------------------------------------------------------------------------
  sram #(
    .Depth     (16),
    .DataWidth (8),
    .ByteWidth (8),
    .ImplKey   ("coeff")
  ) u_mem_a (
    .clk_i   (clk_i),
    .req_i   (mem_a_req),
    .we_i    (mem_a_we_i),
    .addr_i  (mem_a_addr),
    .be_i    (1'b1),
    .wdata_i (mem_a_wdata_i),
    .rdata_o1(mem_a_rdata)
  );

  // ---------------------------------------------------------------------------
  // Memory B: 16-bit x 8 deep (results)
  // ---------------------------------------------------------------------------
  sram #(
    .Depth     (8),
    .DataWidth (16),
    .ByteWidth (8),
    .ImplKey   ("result")
  ) u_mem_b (
    .clk_i   (clk_i),
    .req_i   (mem_b_req),
    .we_i    (mem_b_we),
    .addr_i  (mem_b_addr),
    .be_i    (2'b11),
    .wdata_i (mem_b_wdata),
    .rdata_o1(mem_b_rdata)
  );

  // ---------------------------------------------------------------------------
  // Memory interface muxing (external vs internal access)
  // ---------------------------------------------------------------------------
  always_comb begin
    // Default: external access
    mem_a_req   = mem_a_we_i;
    mem_a_addr  = mem_a_addr_i;
    mem_b_req   = mem_b_we_i;
    mem_b_we    = mem_b_we_i;
    mem_b_addr  = mem_b_addr_i;
    mem_b_wdata = mem_b_wdata_i;

    // Internal access during operation
    if (state_q != StIdle) begin
      mem_a_req   = (state_q == StRead);
      mem_a_addr  = iter_q;
      mem_b_req   = (state_q == StDone);
      mem_b_we    = (state_q == StDone);
      mem_b_addr  = 3'd0;  // Store final result at address 0
      mem_b_wdata = accum_q;
    end
  end

  // ---------------------------------------------------------------------------
  // Combinational next-state logic
  // ---------------------------------------------------------------------------
  always_comb begin
    state_d = state_q;
    iter_d  = iter_q;
    accum_d = accum_q;
    coeff_d = coeff_q;

    unique case (state_q)
      StIdle: begin
        if (start_i) begin
          iter_d  = 4'd0;
          accum_d = 16'd0;
          state_d = StRead;
        end
      end

      StRead: begin
        // Memory read initiated, wait for data
        state_d = StAccum;
      end

      StAccum: begin
        // Accumulate coefficient (zero-extended)
        coeff_d = mem_a_rdata;
        accum_d = accum_q + {8'd0, mem_a_rdata};

        if (iter_q == 4'd15) begin
          // All coefficients processed
          state_d = StDone;
        end else begin
          // Read next coefficient
          iter_d  = iter_q + 4'd1;
          state_d = StRead;
        end
      end

      StDone: begin
        // Write result to memory B, then go idle
        state_d = StIdle;
      end

      default: state_d = StIdle;
    endcase
  end

  // ---------------------------------------------------------------------------
  // Sequential logic
  // ---------------------------------------------------------------------------
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      state_q <= StIdle;
      iter_q  <= 4'd0;
      accum_q <= 16'd0;
      coeff_q <= 8'd0;
    end else begin
      state_q <= state_d;
      iter_q  <= iter_d;
      accum_q <= accum_d;
      coeff_q <= coeff_d;
    end
  end

  // ---------------------------------------------------------------------------
  // Output assignments
  // ---------------------------------------------------------------------------
  assign busy_o        = (state_q != StIdle);
  assign done_o        = (state_q == StDone);
  assign accumulator_o = accum_q;
  assign iteration_o   = iter_q;
  assign mem_a_rdata_o = mem_a_rdata;
  assign mem_b_rdata_o = mem_b_rdata;

endmodule
