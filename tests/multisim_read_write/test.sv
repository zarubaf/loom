module test;

    reg clk;
    // tbx clkgen
    initial begin
        clk = 0;
        forever #1 clk = ~clk;
    end

    reg a;
    always @(posedge clk) begin
        a <= 1;
        @(posedge clk);
        a <= 0;
    end

    initial begin
        $dumpfile("test.fst");
        $dumpvars();
    end
endmodule
