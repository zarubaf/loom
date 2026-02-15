// SPDX-License-Identifier: Apache-2.0
// Copyright Loom contributors.
//
// tb_mem_scan.sv - E2E testbench for memory scan capture
//
// Tests:
// 1. Initialize two SRAMs with known data patterns
// 2. Run memory scan capture
// 3. Verify captured data matches expected values

`timescale 1ns/1ps

module tb_mem_scan;

  // ---------------------------------------------------------------------------
  // Parameters - match loom_mem_scan_ctrl
  // ---------------------------------------------------------------------------
  localparam int unsigned Mem0Depth = 16;
  localparam int unsigned Mem0Width = 8;
  localparam int unsigned Mem1Depth = 8;
  localparam int unsigned Mem1Width = 16;

  localparam int unsigned Mem0AddrWidth = $clog2(Mem0Depth);
  localparam int unsigned Mem1AddrWidth = $clog2(Mem1Depth);

  // Total bits: 16*8 + 8*16 = 128 + 128 = 256 bits
  localparam int unsigned TotalBits = (Mem0Depth * Mem0Width) + (Mem1Depth * Mem1Width);

  // ---------------------------------------------------------------------------
  // Signals
  // ---------------------------------------------------------------------------
  logic clk;
  logic rst_n;

  // Memory 0 interface
  logic                     mem0_req;
  logic                     mem0_we;
  logic [Mem0AddrWidth-1:0] mem0_addr;
  logic [Mem0Width-1:0]     mem0_wdata;
  logic [Mem0Width-1:0]     mem0_rdata;

  // Memory 1 interface
  logic                     mem1_req;
  logic                     mem1_we;
  logic [Mem1AddrWidth-1:0] mem1_addr;
  logic [Mem1Width-1:0]     mem1_wdata;
  logic [Mem1Width-1:0]     mem1_rdata;

  // Scan controller interface
  logic                     scan_cmd_valid;
  logic [1:0]               scan_cmd;
  logic                     scan_busy;
  logic                     scan_done;
  logic                     scan_out;
  logic                     scan_valid;
  logic                     clk_gate_en;

  // Scan controller memory interfaces
  logic                     ctrl_mem0_req;
  logic [Mem0AddrWidth-1:0] ctrl_mem0_addr;
  logic                     ctrl_mem1_req;
  logic [Mem1AddrWidth-1:0] ctrl_mem1_addr;

  // Captured data storage
  logic [TotalBits-1:0]     captured_data;
  int                       capture_idx;

  // Expected data
  logic [Mem0Width-1:0]     expected_mem0 [Mem0Depth];
  logic [Mem1Width-1:0]     expected_mem1 [Mem1Depth];

  // Variables for verification (declared at module level for iverilog)
  logic [TotalBits-1:0]     expected_data;
  integer                   bit_pos;
  integer                   addr_idx;
  integer                   bit_idx;

  // ---------------------------------------------------------------------------
  // Clock generation (100 MHz)
  // ---------------------------------------------------------------------------
  initial clk = 0;
  always #5 clk = ~clk;

  // ---------------------------------------------------------------------------
  // Memory 0: 8-bit x 16 deep
  // ---------------------------------------------------------------------------
  sram #(
    .Depth     (Mem0Depth),
    .DataWidth (Mem0Width),
    .ByteWidth (Mem0Width),
    .ImplKey   ("mem0")
  ) u_mem0 (
    .clk_i   (clk),
    .req_i   (mem0_req),
    .we_i    (mem0_we),
    .addr_i  (mem0_addr),
    .be_i    (1'b1),
    .wdata_i (mem0_wdata),
    .rdata_o1(mem0_rdata)
  );

  // ---------------------------------------------------------------------------
  // Memory 1: 16-bit x 8 deep
  // ---------------------------------------------------------------------------
  sram #(
    .Depth     (Mem1Depth),
    .DataWidth (Mem1Width),
    .ByteWidth (Mem1Width),
    .ImplKey   ("mem1")
  ) u_mem1 (
    .clk_i   (clk),
    .req_i   (mem1_req),
    .we_i    (mem1_we),
    .addr_i  (mem1_addr),
    .be_i    (1'b1),
    .wdata_i (mem1_wdata),
    .rdata_o1(mem1_rdata)
  );

  // ---------------------------------------------------------------------------
  // Memory scan controller
  // ---------------------------------------------------------------------------
  loom_mem_scan_ctrl #(
    .Mem0Depth (Mem0Depth),
    .Mem0Width (Mem0Width),
    .Mem1Depth (Mem1Depth),
    .Mem1Width (Mem1Width)
  ) u_scan_ctrl (
    .clk_i         (clk),
    .rst_ni        (rst_n),
    .cmd_valid_i   (scan_cmd_valid),
    .cmd_i         (scan_cmd),
    .busy_o        (scan_busy),
    .done_o        (scan_done),
    .mem0_req_o    (ctrl_mem0_req),
    .mem0_addr_o   (ctrl_mem0_addr),
    .mem0_rdata_i  (mem0_rdata),
    .mem1_req_o    (ctrl_mem1_req),
    .mem1_addr_o   (ctrl_mem1_addr),
    .mem1_rdata_i  (mem1_rdata),
    .scan_out_o    (scan_out),
    .scan_valid_o  (scan_valid),
    .clk_gate_en_o (clk_gate_en)
  );

  // ---------------------------------------------------------------------------
  // Memory interface muxing (testbench write vs scan read)
  // ---------------------------------------------------------------------------
  // During scan, controller drives memory interface
  // Otherwise, testbench has control
  logic tb_mem0_req, tb_mem1_req;
  logic tb_mem0_we, tb_mem1_we;
  logic [Mem0AddrWidth-1:0] tb_mem0_addr;
  logic [Mem1AddrWidth-1:0] tb_mem1_addr;

  assign mem0_req  = scan_busy ? ctrl_mem0_req : tb_mem0_req;
  assign mem0_we   = scan_busy ? 1'b0 : tb_mem0_we;
  assign mem0_addr = scan_busy ? ctrl_mem0_addr : tb_mem0_addr;

  assign mem1_req  = scan_busy ? ctrl_mem1_req : tb_mem1_req;
  assign mem1_we   = scan_busy ? 1'b0 : tb_mem1_we;
  assign mem1_addr = scan_busy ? ctrl_mem1_addr : tb_mem1_addr;

  // ---------------------------------------------------------------------------
  // Command codes
  // ---------------------------------------------------------------------------
  localparam logic [1:0] CmdNop     = 2'd0;
  localparam logic [1:0] CmdCapture = 2'd1;

  // ---------------------------------------------------------------------------
  // Task: Write to memory 0
  // ---------------------------------------------------------------------------
  task automatic write_mem0(input logic [Mem0AddrWidth-1:0] addr, input logic [Mem0Width-1:0] data);
    @(posedge clk);
    tb_mem0_req  <= 1'b1;
    tb_mem0_we   <= 1'b1;
    tb_mem0_addr <= addr;
    mem0_wdata   <= data;
    @(posedge clk);
    tb_mem0_req  <= 1'b0;
    tb_mem0_we   <= 1'b0;
  endtask

  // ---------------------------------------------------------------------------
  // Task: Write to memory 1
  // ---------------------------------------------------------------------------
  task automatic write_mem1(input logic [Mem1AddrWidth-1:0] addr, input logic [Mem1Width-1:0] data);
    @(posedge clk);
    tb_mem1_req  <= 1'b1;
    tb_mem1_we   <= 1'b1;
    tb_mem1_addr <= addr;
    mem1_wdata   <= data;
    @(posedge clk);
    tb_mem1_req  <= 1'b0;
    tb_mem1_we   <= 1'b0;
  endtask

  // ---------------------------------------------------------------------------
  // Capture scan output
  // ---------------------------------------------------------------------------
  always @(posedge clk) begin
    if (scan_valid) begin
      // Shift in MSB first, so left-shift and put new bit at LSB
      captured_data <= {captured_data[TotalBits-2:0], scan_out};
      capture_idx   <= capture_idx + 1;
    end
  end

  // ---------------------------------------------------------------------------
  // Test sequence
  // ---------------------------------------------------------------------------
  initial begin
    $dumpfile("tb_mem_scan.vcd");
    $dumpvars(0, tb_mem_scan);

    // Initialize signals
    rst_n          = 0;
    scan_cmd_valid = 0;
    scan_cmd       = CmdNop;
    tb_mem0_req    = 0;
    tb_mem0_we     = 0;
    tb_mem0_addr   = 0;
    mem0_wdata     = 0;
    tb_mem1_req    = 0;
    tb_mem1_we     = 0;
    tb_mem1_addr   = 0;
    mem1_wdata     = 0;
    captured_data  = 0;
    capture_idx    = 0;

    // Reset sequence
    repeat (10) @(posedge clk);
    rst_n = 1;
    repeat (5) @(posedge clk);

    // ========================================
    // Test 1: Initialize memories with test data
    // ========================================
    $display("\n=== Test 1: Initialize memories ===");

    // Memory 0: addr as data (8-bit)
    for (addr_idx = 0; addr_idx < Mem0Depth; addr_idx = addr_idx + 1) begin
      expected_mem0[addr_idx] = addr_idx[Mem0Width-1:0];
      write_mem0(addr_idx[Mem0AddrWidth-1:0], addr_idx[Mem0Width-1:0]);
    end
    $display("[TEST] Memory 0 initialized: 16 words of 8 bits each");

    // Memory 1: addr * 256 + addr as data (16-bit)
    for (addr_idx = 0; addr_idx < Mem1Depth; addr_idx = addr_idx + 1) begin
      expected_mem1[addr_idx] = {addr_idx[7:0], addr_idx[7:0]};  // Pattern: high byte = low byte = addr
      write_mem1(addr_idx[Mem1AddrWidth-1:0], {addr_idx[7:0], addr_idx[7:0]});
    end
    $display("[TEST] Memory 1 initialized: 8 words of 16 bits each");

    repeat (10) @(posedge clk);

    // ========================================
    // Test 2: Capture memory contents
    // ========================================
    $display("\n=== Test 2: Capture memory contents ===");

    capture_idx = 0;
    captured_data = 0;

    // Issue capture command
    @(posedge clk);
    scan_cmd_valid <= 1'b1;
    scan_cmd       <= CmdCapture;
    @(posedge clk);
    scan_cmd_valid <= 1'b0;

    // Wait for completion
    $display("[TEST] Capture started, waiting for completion...");
    wait(scan_done);
    @(posedge clk);

    $display("[TEST] Capture complete. Captured %0d bits", capture_idx);
    $display("[TEST] Captured data (hex): 0x%h", captured_data);

    // ========================================
    // Test 3: Verify captured data
    // ========================================
    $display("\n=== Test 3: Verify captured data ===");

    // Build expected data
    // Memory 0 is scanned first (addresses 0-15, 8 bits each)
    // Memory 1 is scanned second (addresses 0-7, 16 bits each)
    // Data is captured MSB first within each word
    // Words are captured in address order
    expected_data = 0;
    bit_pos = TotalBits - 1;

    // Memory 0: 16 words of 8 bits
    for (addr_idx = 0; addr_idx < Mem0Depth; addr_idx = addr_idx + 1) begin
      for (bit_idx = Mem0Width - 1; bit_idx >= 0; bit_idx = bit_idx - 1) begin
        expected_data[bit_pos] = expected_mem0[addr_idx][bit_idx];
        bit_pos = bit_pos - 1;
      end
    end

    // Memory 1: 8 words of 16 bits
    for (addr_idx = 0; addr_idx < Mem1Depth; addr_idx = addr_idx + 1) begin
      for (bit_idx = Mem1Width - 1; bit_idx >= 0; bit_idx = bit_idx - 1) begin
        expected_data[bit_pos] = expected_mem1[addr_idx][bit_idx];
        bit_pos = bit_pos - 1;
      end
    end

    $display("[TEST] Expected data (hex): 0x%h", expected_data);

    if (captured_data == expected_data) begin
      $display("PASS: Captured data matches expected!");
    end else begin
      $display("FAIL: Captured data mismatch!");
      $display("      Expected: 0x%h", expected_data);
      $display("      Got:      0x%h", captured_data);

      // Show memory 0 contents for debugging
      $display("\n[DEBUG] Memory 0 expected words:");
      for (addr_idx = 0; addr_idx < Mem0Depth; addr_idx = addr_idx + 1) begin
        $display("        [%2d] = 0x%02h", addr_idx, expected_mem0[addr_idx]);
      end

      // Show memory 1 contents for debugging
      $display("\n[DEBUG] Memory 1 expected words:");
      for (addr_idx = 0; addr_idx < Mem1Depth; addr_idx = addr_idx + 1) begin
        $display("        [%2d] = 0x%04h", addr_idx, expected_mem1[addr_idx]);
      end
    end

    repeat (10) @(posedge clk);

    $display("\n=== All tests complete ===");
    $finish;
  end

  // ---------------------------------------------------------------------------
  // Timeout watchdog
  // ---------------------------------------------------------------------------
  initial begin
    #100000;
    $display("ERROR: Timeout - simulation did not complete");
    $finish;
  end

endmodule
