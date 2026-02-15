// SPDX-License-Identifier: Apache-2.0
// Copyright lowRISC contributors.
// Copyright Loom contributors.
//
// tb_scan_counter.sv - E2E testbench for scan chain capture/restore
//
// Tests:
// 1. Run counters to build up state
// 2. Capture state via scan chain (clock halted during scan setup)
// 3. Reset counters
// 4. Restore state via scan chain
// 5. Verify state was correctly restored
// 6. Continue counting from restored state

`timescale 1ns/1ps

module tb_scan_counter;

  // ---------------------------------------------------------------------------
  // Parameters
  // ---------------------------------------------------------------------------
  // Chain: wrapped_b(1) + prev_b(16) + counter_b(16) +
  //        wrapped_a(1) + prev_a(8) + counter_a(8) = 50 bits
  localparam int unsigned ScanChainLength = 50;
  localparam int unsigned ScanDataWidth = 64;

  // ---------------------------------------------------------------------------
  // Signals
  // ---------------------------------------------------------------------------
  // Clock and reset
  logic clk;
  logic rst;
  logic rst_n;

  // Gated clock to DUT
  logic clk_gated;
  logic clk_gate_en;

  // Counter interface
  logic        en_a, en_b;
  logic        load_a, load_b;
  logic [7:0]  load_val_a;
  logic [15:0] load_val_b;
  logic [7:0]  count_a;
  logic [15:0] count_b;
  logic        overflow_a, overflow_b;

  // Scan interface to DUT
  logic scan_enable;
  logic scan_in;
  logic scan_out;

  // Scan controller command interface
  logic                      scan_cmd_valid;
  logic [2:0]                scan_cmd;
  logic [15:0]               scan_shift_count;
  logic [ScanDataWidth-1:0]  scan_data_in;
  logic [ScanDataWidth-1:0]  scan_data_out;
  logic                      scan_busy;
  logic                      scan_done;

  // Test storage
  logic [255:0] captured_state;
  logic [7:0]   saved_count_a;
  logic [15:0]  saved_count_b;
  logic         saved_overflow_a, saved_overflow_b;

  // ---------------------------------------------------------------------------
  // Clock generation (100 MHz)
  // ---------------------------------------------------------------------------
  initial clk = 0;
  always #5 clk = ~clk;

  // Active-low reset for scan controller
  assign rst_n = ~rst;

  // ---------------------------------------------------------------------------
  // Clock gate controlled by scan controller
  // ---------------------------------------------------------------------------
  loom_clk_gate u_clk_gate (
    .clk_i (clk),
    .ce_i  (clk_gate_en),
    .clk_o (clk_gated)
  );

  // ---------------------------------------------------------------------------
  // DUT: transformed multi_counter (instantiate directly, not via emu_top)
  // ---------------------------------------------------------------------------
  multi_counter u_dut (
    .clk              (clk_gated),
    .rst              (rst),
    .en_a             (en_a),
    .en_b             (en_b),
    .load_a           (load_a),
    .load_b           (load_b),
    .load_val_a       (load_val_a),
    .load_val_b       (load_val_b),
    .count_a          (count_a),
    .count_b          (count_b),
    .overflow_a       (overflow_a),
    .overflow_b       (overflow_b),
    .loom_scan_enable (scan_enable),
    .loom_scan_in     (scan_in),
    .loom_scan_out    (scan_out)
  );

  // ---------------------------------------------------------------------------
  // Scan controller (lowRISC-style interface)
  // ---------------------------------------------------------------------------
  loom_scan_ctrl #(
    .DataWidth (ScanDataWidth)
  ) u_scan_ctrl (
    .clk_i          (clk),
    .rst_ni         (rst_n),
    // Command interface
    .cmd_valid_i    (scan_cmd_valid),
    .cmd_i          (scan_cmd),
    .shift_count_i  (scan_shift_count),
    .shift_data_i   (scan_data_in),
    .shift_data_o   (scan_data_out),
    .busy_o         (scan_busy),
    .done_o         (scan_done),
    // Scan chain interface
    .scan_enable_o  (scan_enable),
    .scan_in_o      (scan_in),
    .scan_out_i     (scan_out),
    // Clock gate control
    .clk_gate_en_o  (clk_gate_en)
  );

  // ---------------------------------------------------------------------------
  // Command codes (match loom_scan_ctrl)
  // ---------------------------------------------------------------------------
  localparam logic [2:0] CmdNop     = 3'd0;
  localparam logic [2:0] CmdCapture = 3'd1;
  localparam logic [2:0] CmdRestore = 3'd2;

  // ---------------------------------------------------------------------------
  // Task: Capture scan chain state
  // ---------------------------------------------------------------------------
  task automatic capture_scan_state(input int num_bits, output logic [255:0] state);
    state = '0;

    $display("[TEST] Capturing %0d bits via scan chain", num_bits);

    // Issue capture command
    @(posedge clk);
    scan_cmd_valid <= 1'b1;
    scan_cmd <= CmdCapture;
    scan_shift_count <= num_bits[15:0];
    scan_data_in <= '0;

    @(posedge clk);
    scan_cmd_valid <= 1'b0;

    // Wait for completion
    wait(scan_done);
    @(posedge clk);

    // Retrieve captured data
    // Left-shift capture stores data right-aligned: chain[i] at position i
    state[ScanDataWidth-1:0] = scan_data_out;

    $display("[TEST] Capture complete: 0x%h", state[ScanChainLength-1:0]);
  endtask

  // ---------------------------------------------------------------------------
  // Task: Restore scan chain state
  // ---------------------------------------------------------------------------
  task automatic restore_scan_state(input int num_bits, input logic [255:0] state);
    logic [ScanDataWidth-1:0] aligned_data;

    $display("[TEST] Restoring %0d bits via scan chain", num_bits);

    // Left-align the data: shift left by (DataWidth - num_bits)
    // This puts the MSB of captured data (chain[N-1]) at position DataWidth-1
    // So it gets output first via MsbMask extraction
    aligned_data = state[ScanDataWidth-1:0] << (ScanDataWidth - num_bits);

    // Issue restore command
    @(posedge clk);
    scan_cmd_valid <= 1'b1;
    scan_cmd <= CmdRestore;
    scan_shift_count <= num_bits[15:0];
    scan_data_in <= aligned_data;

    @(posedge clk);
    scan_cmd_valid <= 1'b0;

    // Wait for completion
    wait(scan_done);
    @(posedge clk);

    $display("[TEST] Restore complete");
  endtask

  // ---------------------------------------------------------------------------
  // Test sequence
  // ---------------------------------------------------------------------------
  initial begin
    $dumpfile("tb_scan_counter.vcd");
    $dumpvars(0, tb_scan_counter);

    // Initialize signals
    rst = 1;
    en_a = 0;
    en_b = 0;
    load_a = 0;
    load_b = 0;
    load_val_a = 0;
    load_val_b = 0;
    scan_cmd_valid = 0;
    scan_cmd = CmdNop;
    scan_shift_count = 0;
    scan_data_in = 0;

    // Reset sequence
    repeat (10) @(posedge clk);
    #1;
    rst = 0;
    repeat (5) @(posedge clk);

    // ========================================
    // Test 1: Count up to build state
    // ========================================
    $display("\n=== Test 1: Count up ===");

    #1;
    en_a = 1;
    en_b = 1;

    // Count for 100 cycles
    repeat (100) @(posedge clk);

    #1;
    en_a = 0;
    en_b = 0;
    repeat (5) @(posedge clk);

    // Save counter values for later verification
    saved_count_a = count_a;
    saved_count_b = count_b;
    saved_overflow_a = overflow_a;
    saved_overflow_b = overflow_b;

    $display("[TEST] Counter state: count_a=%0d, count_b=%0d, overflow_a=%b, overflow_b=%b",
             count_a, count_b, overflow_a, overflow_b);

    // ========================================
    // Test 2: Capture state via scan chain
    // ========================================
    $display("\n=== Test 2: Capture scan state ===");

    capture_scan_state(ScanChainLength, captured_state);

    repeat (10) @(posedge clk);

    // ========================================
    // Test 3: Reset and verify state is lost
    // ========================================
    $display("\n=== Test 3: Reset counters ===");

    #1;
    rst = 1;
    repeat (5) @(posedge clk);
    #1;
    rst = 0;
    repeat (5) @(posedge clk);

    $display("[TEST] State after reset: count_a=%0d, count_b=%0d", count_a, count_b);

    if (count_a == 0 && count_b == 0) begin
      $display("PASS: Counters correctly reset to 0");
    end else begin
      $display("FAIL: Counters not properly reset");
    end

    repeat (10) @(posedge clk);

    // ========================================
    // Test 4: Restore state via scan chain
    // ========================================
    $display("\n=== Test 4: Restore scan state ===");

    restore_scan_state(ScanChainLength, captured_state);

    repeat (10) @(posedge clk);

    $display("[TEST] State after restore: count_a=%0d, count_b=%0d, overflow_a=%b, overflow_b=%b",
             count_a, count_b, overflow_a, overflow_b);

    // ========================================
    // Test 5: Verify restored state
    // ========================================
    $display("\n=== Test 5: Verify restored state ===");

    if (count_a == saved_count_a && count_b == saved_count_b) begin
      $display("PASS: Counter values correctly restored!");
      $display("      count_a: expected=%0d, got=%0d", saved_count_a, count_a);
      $display("      count_b: expected=%0d, got=%0d", saved_count_b, count_b);
    end else begin
      $display("FAIL: Counter values not correctly restored");
      $display("      count_a: expected=%0d, got=%0d", saved_count_a, count_a);
      $display("      count_b: expected=%0d, got=%0d", saved_count_b, count_b);
    end

    // ========================================
    // Test 6: Continue counting from restored state
    // ========================================
    $display("\n=== Test 6: Continue counting ===");

    #1;
    en_a = 1;
    en_b = 1;
    repeat (10) @(posedge clk);
    #1;
    en_a = 0;
    en_b = 0;
    repeat (5) @(posedge clk);

    $display("[TEST] After 10 more counts: count_a=%0d (expected %0d), count_b=%0d (expected %0d)",
             count_a, (saved_count_a + 10) & 8'hFF,
             count_b, (saved_count_b + 10) & 16'hFFFF);

    if (count_a == ((saved_count_a + 10) & 8'hFF)) begin
      $display("PASS: count_a correctly incremented from restored state");
    end else begin
      $display("FAIL: count_a did not increment correctly");
    end

    repeat (10) @(posedge clk);

    $display("\n=== All tests complete ===");
    $finish;
  end

  // ---------------------------------------------------------------------------
  // Timeout watchdog
  // ---------------------------------------------------------------------------
  initial begin
    #500000;
    $display("ERROR: Timeout - simulation did not complete");
    $finish;
  end

endmodule
