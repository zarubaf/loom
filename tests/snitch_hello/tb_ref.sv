// SPDX-License-Identifier: Apache-2.0
// Reference testbench (plain Verilator, no Loom)

module tb_ref;

    logic clk = 1'b0;
    logic rst_n = 1'b0;

    snitch_hello_top dut (
        .clk_i  (clk),
        .rst_ni (rst_n)
    );

    // Clock: 100 MHz
    always #5ns clk = ~clk;

    // Reset sequence
    initial begin
        rst_n = 1'b0;
        #100ns;
        rst_n = 1'b1;
    end

    // Timeout
    initial begin
        #500_000_000ns;
        $display("ERROR: Simulation timeout!");
        $finish(1);
    end

endmodule
