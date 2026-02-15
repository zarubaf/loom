// SPDX-License-Identifier: Apache-2.0
// loom_host_stub.sv - Simulation stub for host-side DPI handling
//
// This module implements DPI function behavior in Verilog for simulation.
// It monitors loom_dpi_valid, computes results, and signals completion via dpi_ack.
//
// For real hardware, this is replaced by loom_host_pcie (Xilinx XDMA).
//
// Clock gating in emu_top uses: CE = !dpi_valid | dpi_ack
// When dpi_valid=1 and dpi_ack=0, clock is gated.
// Host asserts dpi_ack when result is ready, releasing the clock.

module loom_host_stub #(
    parameter int FUNC_ID_WIDTH = 8,
    parameter int MAX_ARG_WIDTH = 512,
    parameter int MAX_RET_WIDTH = 64,
    // Simulated host latency in clock cycles (0 = immediate)
    parameter int HOST_LATENCY = 10
)(
    input  logic                       clk,
    input  logic                       rst,

    // DPI signals from transformed design
    input  logic                       dpi_valid,
    input  logic [FUNC_ID_WIDTH-1:0]   dpi_func_id,
    input  logic [MAX_ARG_WIDTH-1:0]   dpi_args,
    output logic [MAX_RET_WIDTH-1:0]   dpi_result,

    // DPI acknowledgment - asserted when result is ready
    // emu_top clock gate: CE = !dpi_valid | dpi_ack
    output logic                       dpi_ack
);

    // State machine
    typedef enum logic [1:0] {
        IDLE,
        PROCESSING,
        DONE
    } state_t;

    state_t state, state_next;
    logic [15:0] latency_cnt;
    logic [MAX_RET_WIDTH-1:0] computed_result;

    // State register
    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            state <= IDLE;
            latency_cnt <= '0;
        end else begin
            state <= state_next;

            if (state == PROCESSING) begin
                latency_cnt <= latency_cnt + 1;
            end else begin
                latency_cnt <= '0;
            end
        end
    end

    // Result is available combinationally (ready when dpi_ack is asserted)
    assign dpi_result = computed_result;

    // Next state logic
    always_comb begin
        state_next = state;
        dpi_ack = 1'b0;  // Default: no acknowledgment

        case (state)
            IDLE: begin
                if (dpi_valid) begin
                    state_next = PROCESSING;
                    // dpi_ack = 0, so clock is gated (CE = !valid | ack = 0)
                end
            end

            PROCESSING: begin
                // Still processing, dpi_ack = 0
                if (latency_cnt >= HOST_LATENCY) begin
                    state_next = DONE;
                end
            end

            DONE: begin
                // Result is ready, assert dpi_ack to release clock
                dpi_ack = 1'b1;
                state_next = IDLE;
            end

            default: state_next = IDLE;
        endcase
    end

    // DPI function implementation (Verilog model)
    // TODO: This should be extensible/configurable per design
    always_comb begin
        computed_result = '0;

        case (dpi_func_id)
            // Function 0: dpi_add(int a, int b) -> int
            // Args layout: {b[31:0], a[31:0]}
            8'd0: begin
                // Add a[31:0] + b[63:32]
                computed_result = dpi_args[31:0] + dpi_args[63:32];
            end

            // Function 1: dpi_read_mem(int addr) -> int
            // TODO: Implement memory model
            8'd1: begin
                computed_result = 32'hDEADBEEF;  // Placeholder
            end

            // Function 2: dpi_write_mem(int addr, int data) -> void
            // TODO: Implement memory model
            8'd2: begin
                computed_result = '0;  // void return
            end

            // Add more DPI functions as needed
            default: begin
                computed_result = '0;
            end
        endcase
    end

    // Debug/monitoring
    // synthesis translate_off
    always @(posedge dpi_valid) begin
        $display("[LOOM HOST STUB] DPI call: func_id=%0d, args=%h", dpi_func_id, dpi_args);
    end

    always @(posedge clk) begin
        if (state == DONE) begin
            $display("[LOOM HOST STUB] DPI result: %h", computed_result);
        end
    end
    // synthesis translate_on

endmodule
