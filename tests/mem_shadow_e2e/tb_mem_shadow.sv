// SPDX-License-Identifier: Apache-2.0
// Testbench for mem_shadow pass - verifies unified shadow port interface

module tb_mem_shadow;
    // Design signals
    logic        clk;
    logic        rst_n;

    // Memory A normal interface (8-bit x 256)
    logic        mem_a_we;
    logic [7:0]  mem_a_addr;
    logic [7:0]  mem_a_wdata;
    logic [7:0]  mem_a_rdata;

    // Memory B normal interface (16-bit x 64)
    logic        mem_b_we;
    logic [5:0]  mem_b_addr;
    logic [15:0] mem_b_wdata;
    logic [15:0] mem_b_rdata;

    // Unified shadow interface
    logic        loom_shadow_clk;
    logic [10:0] loom_shadow_addr;   // Global byte address
    logic [15:0] loom_shadow_wdata;  // Padded to max width
    logic [15:0] loom_shadow_rdata;
    logic        loom_shadow_wen;
    logic        loom_shadow_ren;

    // DUT
    mem_test dut (
        .clk(clk),
        .rst_n(rst_n),
        .mem_a_we(mem_a_we),
        .mem_a_addr(mem_a_addr),
        .mem_a_wdata(mem_a_wdata),
        .mem_a_rdata(mem_a_rdata),
        .mem_b_we(mem_b_we),
        .mem_b_addr(mem_b_addr),
        .mem_b_wdata(mem_b_wdata),
        .mem_b_rdata(mem_b_rdata),
        .loom_shadow_clk(loom_shadow_clk),
        .loom_shadow_addr(loom_shadow_addr),
        .loom_shadow_wdata(loom_shadow_wdata),
        .loom_shadow_rdata(loom_shadow_rdata),
        .loom_shadow_wen(loom_shadow_wen),
        .loom_shadow_ren(loom_shadow_ren)
    );

    // Clock generation
    initial clk = 0;
    always #5 clk = ~clk;

    initial loom_shadow_clk = 0;
    always #5 loom_shadow_clk = ~loom_shadow_clk;

    // Test sequences
    int errors = 0;

    // Memory map from the pass (after flatten, order may vary):
    // u_mem_b.u_sram.mem (16-bit x 64): base=0x000, end=0x100 (256 bytes)
    // mem_a (8-bit x 256): base=0x100, end=0x500 (1024 bytes)
    //
    // global_addr = base + (local_addr << 2)

    // Convert mem_a local address to global byte address
    function automatic logic [10:0] mem_a_to_global(logic [7:0] local_addr);
        return 11'h100 + {1'b0, local_addr, 2'b00};  // base=0x100, shift left by 2
    endfunction

    // Convert mem_b local address to global byte address
    function automatic logic [10:0] mem_b_to_global(logic [5:0] local_addr);
        return {3'b000, local_addr, 2'b00};  // base=0x000, shift left by 2
    endfunction

    task automatic write_normal_a(input [7:0] addr, input [7:0] data);
        @(posedge clk);
        mem_a_we <= 1;
        mem_a_addr <= addr;
        mem_a_wdata <= data;
        @(posedge clk);
        mem_a_we <= 0;
    endtask

    task automatic read_normal_a(input [7:0] addr, output [7:0] data);
        @(posedge clk);
        mem_a_we <= 0;
        mem_a_addr <= addr;
        @(posedge clk);
        @(posedge clk);
        data = mem_a_rdata;
    endtask

    task automatic write_shadow(input [10:0] addr, input [15:0] data);
        @(posedge loom_shadow_clk);
        loom_shadow_wen <= 1;
        loom_shadow_addr <= addr;
        loom_shadow_wdata <= data;
        @(posedge loom_shadow_clk);
        loom_shadow_wen <= 0;
    endtask

    task automatic read_shadow(input [10:0] addr, output [15:0] data);
        @(posedge loom_shadow_clk);
        loom_shadow_ren <= 1;
        loom_shadow_addr <= addr;
        @(posedge loom_shadow_clk);
        loom_shadow_ren <= 0;
        @(posedge loom_shadow_clk);
        data = loom_shadow_rdata;
    endtask

    task automatic write_normal_b(input [5:0] addr, input [15:0] data);
        @(posedge clk);
        mem_b_we <= 1;
        mem_b_addr <= addr;
        mem_b_wdata <= data;
        @(posedge clk);
        mem_b_we <= 0;
    endtask

    task automatic read_normal_b(input [5:0] addr, output [15:0] data);
        @(posedge clk);
        mem_b_we <= 0;
        mem_b_addr <= addr;
        @(posedge clk);
        @(posedge clk);
        data = mem_b_rdata;
    endtask

    initial begin
        logic [15:0] rdata;
        logic [7:0]  rdata_a;
        logic [15:0] rdata_b;

        $display("=== mem_shadow E2E Test (Unified Interface) ===");

        // Initialize
        rst_n = 0;
        mem_a_we = 0;
        mem_a_addr = 0;
        mem_a_wdata = 0;
        mem_b_we = 0;
        mem_b_addr = 0;
        mem_b_wdata = 0;
        loom_shadow_wen = 0;
        loom_shadow_ren = 0;
        loom_shadow_addr = 0;
        loom_shadow_wdata = 0;

        repeat(5) @(posedge clk);
        rst_n = 1;
        repeat(2) @(posedge clk);

        // ====== Test 1: Write via normal port, read via shadow (mem_a) ======
        $display("\n[Test 1] Write normal -> Read shadow (mem_a, 8-bit)");

        write_normal_a(8'h10, 8'hAB);
        write_normal_a(8'h20, 8'hCD);
        write_normal_a(8'h30, 8'hEF);

        // Read via shadow interface using global addresses
        read_shadow(mem_a_to_global(8'h10), rdata);
        if (rdata[7:0] !== 8'hAB) begin
            $display("ERROR: addr 0x10: expected 0xAB, got 0x%02x", rdata[7:0]);
            errors++;
        end else begin
            $display("  OK: addr 0x10 = 0x%02x (rdata=0x%04x)", rdata[7:0], rdata);
        end

        read_shadow(mem_a_to_global(8'h20), rdata);
        if (rdata[7:0] !== 8'hCD) begin
            $display("ERROR: addr 0x20: expected 0xCD, got 0x%02x", rdata[7:0]);
            errors++;
        end else begin
            $display("  OK: addr 0x20 = 0x%02x", rdata[7:0]);
        end

        read_shadow(mem_a_to_global(8'h30), rdata);
        if (rdata[7:0] !== 8'hEF) begin
            $display("ERROR: addr 0x30: expected 0xEF, got 0x%02x", rdata[7:0]);
            errors++;
        end else begin
            $display("  OK: addr 0x30 = 0x%02x", rdata[7:0]);
        end

        // ====== Test 2: Write via shadow port, read via normal (mem_a) ======
        $display("\n[Test 2] Write shadow -> Read normal (mem_a)");

        write_shadow(mem_a_to_global(8'h40), 16'h0012);
        write_shadow(mem_a_to_global(8'h50), 16'h0034);

        read_normal_a(8'h40, rdata_a);
        if (rdata_a !== 8'h12) begin
            $display("ERROR: addr 0x40: expected 0x12, got 0x%02x", rdata_a);
            errors++;
        end else begin
            $display("  OK: addr 0x40 = 0x%02x", rdata_a);
        end

        read_normal_a(8'h50, rdata_a);
        if (rdata_a !== 8'h34) begin
            $display("ERROR: addr 0x50: expected 0x34, got 0x%02x", rdata_a);
            errors++;
        end else begin
            $display("  OK: addr 0x50 = 0x%02x", rdata_a);
        end

        // ====== Test 3: Memory B (16-bit) via unified interface ======
        $display("\n[Test 3] Write normal -> Read shadow (mem_b, 16-bit)");

        write_normal_b(6'h0A, 16'hDEAD);
        write_normal_b(6'h0B, 16'hBEEF);

        read_shadow(mem_b_to_global(6'h0A), rdata);
        if (rdata !== 16'hDEAD) begin
            $display("ERROR: addr 0x0A: expected 0xDEAD, got 0x%04x", rdata);
            errors++;
        end else begin
            $display("  OK: addr 0x0A = 0x%04x", rdata);
        end

        read_shadow(mem_b_to_global(6'h0B), rdata);
        if (rdata !== 16'hBEEF) begin
            $display("ERROR: addr 0x0B: expected 0xBEEF, got 0x%04x", rdata);
            errors++;
        end else begin
            $display("  OK: addr 0x0B = 0x%04x", rdata);
        end

        // ====== Test 4: Shadow write to mem_b ======
        $display("\n[Test 4] Write shadow -> Read normal (mem_b)");

        write_shadow(mem_b_to_global(6'h3F), 16'hCAFE);

        read_normal_b(6'h3F, rdata_b);
        if (rdata_b !== 16'hCAFE) begin
            $display("ERROR: addr 0x3F: expected 0xCAFE, got 0x%04x", rdata_b);
            errors++;
        end else begin
            $display("  OK: addr 0x3F = 0x%04x", rdata_b);
        end

        // Summary
        $display("\n=== Test Summary ===");
        if (errors == 0) begin
            $display("PASS: All tests passed!");
        end else begin
            $display("FAIL: %d errors", errors);
        end

        $finish;
    end

endmodule
