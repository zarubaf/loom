// SPDX-License-Identifier: Apache-2.0
// loom_host_if.sv - Abstract host communication interface
//
// This interface defines the contract between the FPGA emulation wrapper
// and the host (either a simulation stub or real PCIe).

interface loom_host_if #(
    parameter int FUNC_ID_WIDTH = 8,
    parameter int MAX_ARG_WIDTH = 512,
    parameter int MAX_RET_WIDTH = 64
);

    // DPI call signaling (directly from transformed design)
    logic                       dpi_valid;      // DPI call pending
    logic [FUNC_ID_WIDTH-1:0]   dpi_func_id;    // Function identifier
    logic [MAX_ARG_WIDTH-1:0]   dpi_args;       // Packed arguments
    logic [MAX_RET_WIDTH-1:0]   dpi_result;     // Return value

    // Clock control
    logic                       clk_enable;     // 1=run design, 0=freeze

    // Host communication (memory-mapped style)
    logic                       host_req;       // Host request valid
    logic                       host_wr;        // 1=write, 0=read
    logic [31:0]                host_addr;      // Register address
    logic [63:0]                host_wdata;     // Write data
    logic [63:0]                host_rdata;     // Read data
    logic                       host_ack;       // Request acknowledged

    // Modport for FPGA side (emu_top)
    modport fpga (
        input  dpi_valid, dpi_func_id, dpi_args,
        output dpi_result, clk_enable,
        input  host_req, host_wr, host_addr, host_wdata,
        output host_rdata, host_ack
    );

    // Modport for host side (stub or PCIe bridge)
    modport host (
        output dpi_valid, dpi_func_id, dpi_args,
        input  dpi_result, clk_enable,
        output host_req, host_wr, host_addr, host_wdata,
        input  host_rdata, host_ack
    );

    // Modport for DUT connection (directly wired)
    modport dut (
        output dpi_valid, dpi_func_id, dpi_args,
        input  dpi_result
    );

endinterface
