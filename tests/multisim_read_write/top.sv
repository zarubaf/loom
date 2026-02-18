module top;

  reg clk;
  // tbx clkgen
  initial begin
    clk = 0;
    forever #1 clk = ~clk;
  end

  bit [63:0] mem[1<<8];

  //-----------------------------------------------------------
  // exit
  //-----------------------------------------------------------
  bit exit;

  multisim_server_pull #(
      .DATA_WIDTH(1),
      .DPI_DELAY_CYCLES_INACTIVE(10000)
  ) i_multisim_server_pull_exit (
      .clk        (clk),
      .server_name("exit"),
      .data_rdy   (1'b1),
      .data_vld   (exit),
      .data       (  /*unused*/)
  );

  always @(posedge clk) begin
    if (exit) begin
      $display("exit");
      $finish;
    end
  end

  //-----------------------------------------------------------
  // read/write
  //-----------------------------------------------------------
  bit rw_cmd_rdy;
  bit rw_cmd_vld;
  bit [3*64-1:0] rw_cmd;

  wire rw_cmd_rwb = rw_cmd[0];
  wire [63:0] rw_cmd_address = rw_cmd[1*64+:64];
  wire [63:0] rw_cmd_wdata = rw_cmd[2*64+:64];

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

  always @(posedge clk) begin
    rw_cmd_rdy <= 1;
    @(posedge clk);
    while (!rw_cmd_vld) begin
      @(posedge clk);
    end
    rw_cmd_rdy <= 0;

    // process
    if (rw_cmd_rwb) begin
      rw_rsp <= mem[rw_cmd_address[7:0]];
    end else begin
      mem[rw_cmd_address[7:0]] <= rw_cmd_wdata;
      rw_rsp <= 0;
    end

    rw_rsp_vld <= 1;
    @(posedge clk);
    while (!rw_rsp_rdy) begin
      @(posedge clk);
    end
    rw_rsp_vld <= 0;
  end

endmodule
