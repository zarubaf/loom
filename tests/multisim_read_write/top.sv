module top;

  reg clk;
  // tbx clkgen
  initial begin
    clk = 0;
    forever #1 clk = ~clk;
  end

  bit [63:0] mem[1<<8];

  //-----------------------------------------------------------
  // read/write/exit — all commands go through a single channel.
  //
  // Previously exit used a separate multisim_server_pull channel.
  // This caused intermittent failures: the exit poll could fire
  // $finish while the rw channel had an in-flight DPI transaction,
  // tearing down sockets before the client received its response.
  // Merging exit into the rw command stream guarantees sequencing.
  //
  //   cmd[0] = 0: write  (address in cmd[1], data in cmd[2])
  //   cmd[0] = 1: read   (address in cmd[1])
  //   cmd[0] = 2: exit
  //-----------------------------------------------------------
  bit rw_cmd_rdy;
  bit rw_cmd_vld;
  bit [3*64-1:0] rw_cmd;

  wire [63:0] rw_cmd_op      = rw_cmd[0*64+:64];
  wire [63:0] rw_cmd_address = rw_cmd[1*64+:64];
  wire [63:0] rw_cmd_wdata   = rw_cmd[2*64+:64];

  bit rw_rsp_rdy;
  bit rw_rsp_vld;
  bit [63:0] rw_rsp;

  multisim_server_pull_then_push #(
      .PULL_DATA_WIDTH(3 * 64),
      .PUSH_DATA_WIDTH(64)
  ) i_multisim_server_rw (
      .clk             (clk),
      // pull
      .pull_server_name("rw_cmd"),
      .pull_data_rdy   (rw_cmd_rdy),
      .pull_data_vld   (rw_cmd_vld),
      .pull_data       (rw_cmd),
      // push
      .push_server_name("rw_rsp"),
      .push_data_rdy   (rw_rsp_rdy),
      .push_data_vld   (rw_rsp_vld),
      .push_data       (rw_rsp)
  );

  // Deferred $finish: set by the rw handler when it receives an exit
  // command (op==2).  Fires on the NEXT cycle so the push DPI has time
  // to deliver the response to the client before the sim tears down.
  bit do_exit = 0;

  always @(posedge clk) begin
    rw_cmd_rdy <= 1;
    @(posedge clk);
    while (!rw_cmd_vld) begin
      @(posedge clk);
    end
    rw_cmd_rdy <= 0;

    // process
    if (rw_cmd_op == 0) begin
      // write
      mem[rw_cmd_address[7:0]] <= rw_cmd_wdata;
      rw_rsp <= 0;
    end else if (rw_cmd_op == 1) begin
      // read
      rw_rsp <= mem[rw_cmd_address[7:0]];
    end else begin
      // exit (op == 2) — respond first, then finish
      rw_rsp <= 0;
    end

    rw_rsp_vld <= 1;
    @(posedge clk);
    while (!rw_rsp_rdy) begin
      @(posedge clk);
    end
    rw_rsp_vld <= 0;

    if (rw_cmd_op == 2) begin
      do_exit <= 1;
    end
  end

  always @(posedge clk) begin
    if (do_exit) begin
      $display("exit");
      $finish;
    end
  end

endmodule
