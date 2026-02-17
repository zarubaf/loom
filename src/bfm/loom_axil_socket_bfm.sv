// SPDX-License-Identifier: Apache-2.0
// Generic AXI-Lite master BFM driven by a Unix domain socket
//
// This module is completely DUT-agnostic and reusable in any project.
// It bridges a Unix domain socket to AXI-Lite transactions.
// Compatible with Verilator --binary --timing.

`timescale 1ns/1ps

module loom_axil_socket_bfm #(
    parameter string SOCKET_PATH = "/tmp/loom_sim.sock",
    parameter int    ADDR_WIDTH  = 20,
    parameter int    N_IRQ       = 16
)(
    input  logic                    clk_i,
    input  logic                    rst_ni,

    // AXI-Lite Master interface
    output logic [ADDR_WIDTH-1:0]   m_axil_araddr_o,
    output logic                    m_axil_arvalid_o,
    input  logic                    m_axil_arready_i,
    input  logic [31:0]             m_axil_rdata_i,
    input  logic [1:0]              m_axil_rresp_i,
    input  logic                    m_axil_rvalid_i,
    output logic                    m_axil_rready_o,

    output logic [ADDR_WIDTH-1:0]   m_axil_awaddr_o,
    output logic                    m_axil_awvalid_o,
    input  logic                    m_axil_awready_i,
    output logic [31:0]             m_axil_wdata_o,
    output logic [3:0]              m_axil_wstrb_o,
    output logic                    m_axil_wvalid_o,
    input  logic                    m_axil_wready_i,
    input  logic [1:0]              m_axil_bresp_i,
    input  logic                    m_axil_bvalid_i,
    output logic                    m_axil_bready_o,

    // Interrupt inputs (forwarded to host on rising edge)
    input  logic [N_IRQ-1:0]        irq_i,

    // Finish input (triggers shutdown sequence)
    input  logic                    finish_i,

    // Shutdown output (high after SHUTDOWN message sent, testbench should call $finish)
    output logic                    shutdown_o
);

    // -------------------------------------------------------------------------
    // DPI-C imports
    // -------------------------------------------------------------------------
    import "DPI-C" function int  loom_sock_init(string path);
    import "DPI-C" function int  loom_sock_try_recv(
        output byte unsigned req_type,
        output int unsigned  req_offset,
        output int unsigned  req_wdata
    );
    import "DPI-C" function void loom_sock_send(
        input byte unsigned  resp_type,
        input int unsigned   rdata,
        input int unsigned   irq_bits
    );
    import "DPI-C" function void loom_sock_close();
    import "DPI-C" function void loom_sock_set_trace(int enable);

    // -------------------------------------------------------------------------
    // State machine
    // -------------------------------------------------------------------------
    typedef enum logic [2:0] {
        StIdle,
        StReadAddr,
        StReadData,
        StWriteAddrData,
        StWriteResp
    } state_e;

    state_e state_q, state_d;
    logic [N_IRQ-1:0] irq_prev_q;
    logic finish_pending_q;
    logic shutdown_sent_q;

    // Runtime trace enable (+loom_bfm_trace on command line)
    logic bfm_trace;

    initial begin
        bfm_trace = 1'b0;
        if ($test$plusargs("loom_bfm_trace")) bfm_trace = 1'b1;
    end

    // Pending request fields (module-level for DPI access)
    byte unsigned  req_type;
    int unsigned   req_offset;
    int unsigned   req_wdata;
    int            recv_rv;

    // Registered versions of request fields (captured when recv_rv==1)
    logic          pending_valid_q;
    logic          pending_is_write_q;
    logic [ADDR_WIDTH-1:0] pending_addr_q;
    logic [31:0]   pending_wdata_q;

    // IRQ edge detection
    logic [N_IRQ-1:0] irq_rising;
    assign irq_rising = irq_i & ~irq_prev_q;

    // Shutdown output - high after SHUTDOWN message has been sent
    assign shutdown_o = shutdown_sent_q;

    // -------------------------------------------------------------------------
    // Socket initialization
    // -------------------------------------------------------------------------
    // Note: Socket init must happen after some simulation time to allow reset
    // to propagate. We use a delayed initial block.
    logic socket_initialized;

    initial begin
        socket_initialized = 1'b0;
        // Wait for reset to complete (100ns at 1ns timescale)
        #200;
        if (loom_sock_init(SOCKET_PATH) < 0) begin
            $error("[loom_bfm] Failed to initialize socket at %s", SOCKET_PATH);
            $finish;
        end
        socket_initialized = 1'b1;
        loom_sock_set_trace(int'(bfm_trace));
    end

    // Cleanup on finish
    final begin
        loom_sock_close();
    end

    // -------------------------------------------------------------------------
    // Combinational next-state logic
    // -------------------------------------------------------------------------
    always_comb begin
        state_d = state_q;

        case (state_q)
            StIdle: begin
                // Transition when we have a pending request (and not shutting down)
                if (pending_valid_q && !shutdown_sent_q) begin
                    if (!pending_is_write_q) begin
                        state_d = StReadAddr;
                    end else begin
                        state_d = StWriteAddrData;
                    end
                end
            end

            StReadAddr: begin
                if (m_axil_arready_i) begin
                    state_d = StReadData;
                end
            end

            StReadData: begin
                if (m_axil_rvalid_i) begin
                    state_d = StIdle;
                end
            end

            StWriteAddrData: begin
                if ((m_axil_awready_i || !m_axil_awvalid_o) &&
                    (m_axil_wready_i  || !m_axil_wvalid_o)) begin
                    state_d = StWriteResp;
                end
            end

            StWriteResp: begin
                if (m_axil_bvalid_i) begin
                    state_d = StIdle;
                end
            end

            default: state_d = StIdle;
        endcase
    end

    // -------------------------------------------------------------------------
    // Sequential logic with DPI calls
    // -------------------------------------------------------------------------
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q            <= StIdle;
            irq_prev_q         <= '0;
            m_axil_arvalid_o   <= 1'b0;
            m_axil_rready_o    <= 1'b0;
            m_axil_awvalid_o   <= 1'b0;
            m_axil_wvalid_o    <= 1'b0;
            m_axil_bready_o    <= 1'b0;
            m_axil_wstrb_o     <= 4'hF;
            m_axil_araddr_o    <= '0;
            m_axil_awaddr_o    <= '0;
            m_axil_wdata_o     <= '0;
            recv_rv            <= 0;
            pending_valid_q    <= 1'b0;
            pending_is_write_q <= 1'b0;
            pending_addr_q     <= '0;
            pending_wdata_q    <= '0;
            finish_pending_q   <= 1'b0;
            shutdown_sent_q    <= 1'b0;
        end else begin
            state_q <= state_d;

            // --- IRQ edge detection ---
            irq_prev_q <= irq_i;
            if (|irq_rising) begin
                loom_sock_send(8'd2, 32'd0, {{(32-N_IRQ){1'b0}}, irq_rising});
            end

            // --- Finish handling (runs every cycle) ---
            if (finish_i && !finish_pending_q) begin
                finish_pending_q <= 1'b1;
            end

            // Send SHUTDOWN message when finish is pending and we're idle
            if (finish_pending_q && !shutdown_sent_q && state_q == StIdle && !pending_valid_q) begin
                if (bfm_trace) $display("[BFM] Sending SHUTDOWN message");
                loom_sock_send(8'd3, 32'd0, 32'd0);  // Type 3 = SHUTDOWN
                shutdown_sent_q <= 1'b1;
            end

            // --- State machine actions ---
            case (state_q)
                StIdle: begin
                    // If we have a pending request from previous cycle, start the AXI transaction
                    if (pending_valid_q) begin
                        pending_valid_q <= 1'b0;  // Clear pending flag
                        if (!pending_is_write_q) begin
                            // READ request
                            if (bfm_trace) $display("[BFM] READ addr=0x%05x", pending_addr_q);
                            m_axil_araddr_o  <= pending_addr_q;
                            m_axil_arvalid_o <= 1'b1;
                        end else begin
                            // WRITE request
                            if (bfm_trace) $display("[BFM] WRITE addr=0x%05x data=0x%08x", pending_addr_q, pending_wdata_q);
                            m_axil_awaddr_o  <= pending_addr_q;
                            m_axil_awvalid_o <= 1'b1;
                            m_axil_wdata_o   <= pending_wdata_q;
                            m_axil_wvalid_o  <= 1'b1;
                        end
                    end else if (!finish_pending_q) begin
                        // Poll for socket messages (only if socket is initialized and not shutting down)
                        if (socket_initialized) begin
                            recv_rv = loom_sock_try_recv(req_type, req_offset, req_wdata);
                            // If we received a message, capture it into pending registers
                            if (recv_rv == 1) begin
                                pending_valid_q    <= 1'b1;
                                pending_is_write_q <= (req_type != 0);
                                pending_addr_q     <= req_offset[ADDR_WIDTH-1:0];
                                pending_wdata_q    <= req_wdata;
                            end
                        end
                    end
                end

                StReadAddr: begin
                    if (m_axil_arready_i) begin
                        m_axil_arvalid_o <= 1'b0;
                        m_axil_rready_o  <= 1'b1;
                    end
                end

                StReadData: begin
                    if (m_axil_rvalid_i) begin
                        if (bfm_trace) $display("[BFM] READ response data=0x%08x", m_axil_rdata_i);
                        m_axil_rready_o <= 1'b0;
                        loom_sock_send(8'd0, m_axil_rdata_i, 32'd0);
                    end
                end

                StWriteAddrData: begin
                    if (m_axil_awready_i) m_axil_awvalid_o <= 1'b0;
                    if (m_axil_wready_i)  m_axil_wvalid_o  <= 1'b0;

                    if ((m_axil_awready_i || !m_axil_awvalid_o) &&
                        (m_axil_wready_i  || !m_axil_wvalid_o)) begin
                        m_axil_bready_o <= 1'b1;
                    end
                end

                StWriteResp: begin
                    if (m_axil_bvalid_i) begin
                        m_axil_bready_o <= 1'b0;
                        loom_sock_send(8'd1, 32'd0, 32'd0);
                    end
                end

                default: ;
            endcase
        end
    end

endmodule
