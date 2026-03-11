// SPDX-License-Identifier: Apache-2.0
// Loom ICAP Controller
//
// AXI-Lite slave that streams bitstream words to ICAPE3 for in-system
// partial reconfiguration via PCIe.
//
// Register Map (offset from base 0x6_0000):
//   0x00  STATUS (R)  [0]=busy, [1]=prdone (sticky), [2]=prerror (sticky)
//   0x04  CTRL   (W)  [0]=sw_reset (clears sticky bits, returns FSM to idle)
//   0x08  DATA   (W)  bitstream word — AXI backpressure holds awready low
//                     while the previous word is being presented to ICAP
//
// Data format:
//   Host writes raw bytes from the .bit file (after stripping the header up
//   to the sync word 0xAA995566) packed little-endian into uint32_t words
//   (file byte 0 → wdata[7:0], byte 1 → [15:8], etc.).  Hardware applies
//   per-byte bit-reversal before ICAPE3 as required by UG570 Table 2-7.
//
// Placement (FPGA):
//   The ICAPE3 site is constrained to ICAP_X0Y0 in u250_dfx.xdc so that
//   the instance stays in the static region.  In simulation the behavioral
//   ICAPE3 stub from src/bfm/xilinx_primitives.sv is used automatically.

`default_nettype none

module loom_icap_ctrl #(
    parameter int unsigned ADDR_WIDTH = 20
)(
    input  logic clk_i,
    input  logic rst_ni,

    input  logic [ADDR_WIDTH-1:0] s_axil_araddr_i,
    input  logic                  s_axil_arvalid_i,
    output logic                  s_axil_arready_o,
    output logic [31:0]           s_axil_rdata_o,
    output logic [1:0]            s_axil_rresp_o,
    output logic                  s_axil_rvalid_o,
    input  logic                  s_axil_rready_i,

    input  logic [ADDR_WIDTH-1:0] s_axil_awaddr_i,
    input  logic                  s_axil_awvalid_i,
    output logic                  s_axil_awready_o,
    input  logic [31:0]           s_axil_wdata_i,
    input  logic [3:0]            s_axil_wstrb_i,
    input  logic                  s_axil_wvalid_i,
    output logic                  s_axil_wready_o,
    output logic [1:0]            s_axil_bresp_o,
    output logic                  s_axil_bvalid_o,
    input  logic                  s_axil_bready_i
);

    // =========================================================================
    // Register offset decode (bits [3:2] of address)
    // =========================================================================
    localparam logic [1:0] REG_STATUS = 2'h0;  // 0x00
    localparam logic [1:0] REG_CTRL   = 2'h1;  // 0x04
    localparam logic [1:0] REG_DATA   = 2'h2;  // 0x08

    // =========================================================================
    // Per-byte bit-reversal (UG570 Table 2-7)
    // =========================================================================
    function automatic [31:0] bit_rev_bytes(input logic [31:0] d);
        for (int b = 0; b < 4; b++)
            for (int i = 0; i < 8; i++)
                bit_rev_bytes[b*8 + i] = d[b*8 + (7-i)];
    endfunction

    // =========================================================================
    // ICAPE3 primitive
    // Resolved at link time: Vivado library on FPGA, xilinx_primitives.sv in sim.
    // =========================================================================
    logic        icap_csib;
    logic [31:0] icap_din;
    logic [31:0] icap_rdata;   // readback not used; named to silence PINCONNECTEMPTY
    logic        icap_avail;
    logic        icap_prdone;
    logic        icap_prerror;

    (* DONT_TOUCH = "yes" *)
    ICAPE3 #(
        .ICAP_AUTO_SWITCH ("DISABLE"),
        .SIM_CFG_FILE_NAME("NONE")
    ) u_icap (
        .CLK    (clk_i),
        .CSIB   (icap_csib),
        .RDWRB  (1'b0),
        .I      (icap_din),
        .O      (icap_rdata),
        .AVAIL  (icap_avail),
        .PRDONE (icap_prdone),
        .PRERROR(icap_prerror)
    );

    // =========================================================================
    // AXI-Lite Write Tracking
    // AW and W channels are tracked separately (per AXI-Lite spec).
    // For DATA register writes awready is suppressed while ICAP is busy,
    // providing natural backpressure one word at a time.
    // =========================================================================
    logic        aw_pending_q;
    logic [1:0]  aw_addr_q;
    logic        w_pending_q;
    logic [31:0] w_data_q;
    logic        bvalid_q;

    // =========================================================================
    // FSM
    // =========================================================================
    typedef enum logic [1:0] {
        StIdle      = 2'b00,
        StWaitAvail = 2'b01,
        StWrite     = 2'b10
    } state_e;

    state_e      state_q, state_d;
    logic [31:0] data_q;    // bit-reversed data staged for ICAP
    logic        prdone_q;  // sticky
    logic        prerror_q; // sticky

    // =========================================================================
    // Combinational logic (unified always_comb)
    // =========================================================================
    logic aw_accepted, w_accepted;
    logic is_data_wr;
    logic both_done;
    logic [1:0] exec_aw_addr;
    logic exec_data, exec_ctrl_reset;

    always_comb begin
        // AW/W handshake
        is_data_wr = (s_axil_awaddr_i[3:2] == REG_DATA);

        // For DATA writes: accept AW only when FSM is idle (backpressure)
        s_axil_awready_o = !aw_pending_q && (!is_data_wr || state_q == StIdle);
        s_axil_wready_o  = !w_pending_q;

        aw_accepted = s_axil_awvalid_i && s_axil_awready_o;
        w_accepted  = s_axil_wvalid_i  && s_axil_wready_o;

        // "both_done": both channels have been captured (either registered or current)
        both_done = (aw_pending_q || aw_accepted) && (w_pending_q || w_accepted);

        // Resolve which AW/W values to act on
        exec_aw_addr = aw_accepted ? s_axil_awaddr_i[3:2] : aw_addr_q;

        // Decode write operation (fire when both channels done and no B pending)
        exec_data       = 1'b0;
        exec_ctrl_reset = 1'b0;
        if (both_done && !bvalid_q) begin
            case (exec_aw_addr)
                REG_DATA: exec_data       = 1'b1;
                REG_CTRL: exec_ctrl_reset = w_accepted ? s_axil_wdata_i[0] : w_data_q[0];
                default:  ;  // STATUS is read-only
            endcase
        end

        // FSM next-state
        state_d   = state_q;
        icap_csib = 1'b1;
        icap_din  = data_q;

        unique case (state_q)
            StIdle: begin
                if (exec_data)
                    state_d = icap_avail ? StWrite : StWaitAvail;
            end
            StWaitAvail: begin
                if (icap_avail) state_d = StWrite;
            end
            StWrite: begin
                icap_csib = 1'b0;
                state_d   = StIdle;
            end
            default: state_d = StIdle;
        endcase
    end

    // =========================================================================
    // Sequential state
    // =========================================================================
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q      <= StIdle;
            data_q       <= '0;
            prdone_q     <= 1'b0;
            prerror_q    <= 1'b0;
            aw_pending_q <= 1'b0;
            aw_addr_q    <= '0;
            w_pending_q  <= 1'b0;
            w_data_q     <= '0;
            bvalid_q     <= 1'b0;
        end else begin
            state_q <= state_d;

            // Stage bit-reversed data for ICAP presentation
            if (exec_data) data_q <= bit_rev_bytes(w_accepted ? s_axil_wdata_i : w_data_q);

            // Sticky status bits
            if (exec_ctrl_reset) begin
                prdone_q  <= 1'b0;
                prerror_q <= 1'b0;
            end else begin
                if (icap_prdone)  prdone_q  <= 1'b1;
                if (icap_prerror) prerror_q <= 1'b1;
            end

            // AXI-Lite write channel tracking
            if (aw_accepted)            begin aw_pending_q <= 1'b1; aw_addr_q <= s_axil_awaddr_i[3:2]; end
            if (w_accepted)             begin w_pending_q  <= 1'b1; w_data_q  <= s_axil_wdata_i;       end
            if (both_done && !bvalid_q) begin
                aw_pending_q <= 1'b0;
                w_pending_q  <= 1'b0;
                bvalid_q     <= 1'b1;
            end
            if (bvalid_q && s_axil_bready_i) bvalid_q <= 1'b0;
        end
    end

    // B response
    assign s_axil_bvalid_o = bvalid_q;
    assign s_axil_bresp_o  = 2'b00;  // OKAY

    // =========================================================================
    // AXI-Lite Read Path
    // =========================================================================
    logic        rvalid_q;
    logic [31:0] rdata_q;

    assign s_axil_arready_o = !rvalid_q || s_axil_rready_i;
    assign s_axil_rvalid_o  = rvalid_q;
    assign s_axil_rdata_o   = rdata_q;
    assign s_axil_rresp_o   = 2'b00;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            rvalid_q <= 1'b0;
            rdata_q  <= '0;
        end else begin
            if (s_axil_arready_o && s_axil_arvalid_i) begin
                rvalid_q <= 1'b1;
                case (s_axil_araddr_i[3:2])
                    REG_STATUS: rdata_q <= {29'b0, prerror_q, prdone_q, (state_q != StIdle)};
                    default:    rdata_q <= '0;
                endcase
            end else if (s_axil_rready_i) begin
                rvalid_q <= 1'b0;
            end
        end
    end

endmodule

`default_nettype wire
