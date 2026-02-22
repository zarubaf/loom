// SPDX-License-Identifier: Apache-2.0
// Loom AXI-Lite Firewall
//
// Sits between the host AXI-Lite master and the emulation domain CDC.
// Provides:
//   - Timeout detection: synthetic response after TIMEOUT_CYCLES idle cycles
//   - Unsolicited response filtering: swallows downstream responses with no
//     matching outstanding request
//   - Back-pressure: limits outstanding transactions to MAX_OUTSTANDING
//   - Lockdown mode: blocks new upstream requests
//   - Decouple control: drives decoupler decouple input
//   - Management register file on a separate AXI-Lite port (s_mgmt)
//
// Management Register Map (offset from s_mgmt base):
//   0x00  CTRL              RW  bit0=lockdown, bit1=clear_counts (W1C), bit2=decouple
//   0x04  STATUS            R   bit0=locked, bit1=wr_outstanding!=0, bit2=rd_outstanding!=0, bit3=decouple_status
//   0x08  TIMEOUT_CYCLES    RW  Timeout countdown init value
//   0x0C  RESP_ON_TIMEOUT   RW  AXI RESP value on timeout [1:0]
//   0x10  RDATA_ON_TIMEOUT  RW  RDATA value on timeout
//   0x14  TIMEOUT_COUNT     R   Number of timeouts since last clear
//   0x18  UNSOLICITED_COUNT R   Number of unsolicited responses swallowed
//   0x1C  MAX_OUTSTANDING   RW  Max outstanding transactions per channel
//   0x20  IRQ_ENABLE        RW  bit0=timeout, bit1=unsolicited

module loom_axil_firewall #(
    parameter int unsigned DATA_WIDTH            = 32,
    parameter int unsigned ADDR_WIDTH            = 32,
    parameter int unsigned TIMEOUT_CYCLES_INIT   = 1000,
    parameter [1:0]        RESP_ON_TIMEOUT_INIT  = 2'b10,
    parameter [31:0]       RDATA_ON_TIMEOUT_INIT = 32'hDEADBEEF,
    parameter int unsigned MAX_OUTSTANDING_INIT  = 4,
    parameter int unsigned FIFO_DEPTH            = 16
)(
    input  logic                    clk_i,
    input  logic                    rst_ni,

    // =====================================================================
    // Upstream AXI-Lite slave (from host / demux master 0)
    // =====================================================================
    input  logic [ADDR_WIDTH-1:0]   s_axi_awaddr,
    input  logic [2:0]              s_axi_awprot,
    input  logic                    s_axi_awvalid,
    output logic                    s_axi_awready,
    input  logic [DATA_WIDTH-1:0]   s_axi_wdata,
    input  logic [(DATA_WIDTH/8)-1:0] s_axi_wstrb,
    input  logic                    s_axi_wvalid,
    output logic                    s_axi_wready,
    output logic [1:0]              s_axi_bresp,
    output logic                    s_axi_bvalid,
    input  logic                    s_axi_bready,

    input  logic [ADDR_WIDTH-1:0]   s_axi_araddr,
    input  logic [2:0]              s_axi_arprot,
    input  logic                    s_axi_arvalid,
    output logic                    s_axi_arready,
    output logic [DATA_WIDTH-1:0]   s_axi_rdata,
    output logic [1:0]              s_axi_rresp,
    output logic                    s_axi_rvalid,
    input  logic                    s_axi_rready,

    // =====================================================================
    // Downstream AXI-Lite master (to CDC → emu_top)
    // =====================================================================
    output logic [ADDR_WIDTH-1:0]   m_axi_awaddr,
    output logic [2:0]              m_axi_awprot,
    output logic                    m_axi_awvalid,
    input  logic                    m_axi_awready,
    output logic [DATA_WIDTH-1:0]   m_axi_wdata,
    output logic [(DATA_WIDTH/8)-1:0] m_axi_wstrb,
    output logic                    m_axi_wvalid,
    input  logic                    m_axi_wready,
    input  logic [1:0]              m_axi_bresp,
    input  logic                    m_axi_bvalid,
    output logic                    m_axi_bready,

    output logic [ADDR_WIDTH-1:0]   m_axi_araddr,
    output logic [2:0]              m_axi_arprot,
    output logic                    m_axi_arvalid,
    input  logic                    m_axi_arready,
    input  logic [DATA_WIDTH-1:0]   m_axi_rdata,
    input  logic [1:0]              m_axi_rresp,
    input  logic                    m_axi_rvalid,
    output logic                    m_axi_rready,

    // =====================================================================
    // Management AXI-Lite slave (from demux master 2)
    // =====================================================================
    input  logic [ADDR_WIDTH-1:0]   s_mgmt_awaddr,
    input  logic                    s_mgmt_awvalid,
    output logic                    s_mgmt_awready,
    input  logic [DATA_WIDTH-1:0]   s_mgmt_wdata,
    input  logic [(DATA_WIDTH/8)-1:0] s_mgmt_wstrb,
    input  logic                    s_mgmt_wvalid,
    output logic                    s_mgmt_wready,
    output logic [1:0]              s_mgmt_bresp,
    output logic                    s_mgmt_bvalid,
    input  logic                    s_mgmt_bready,

    input  logic [ADDR_WIDTH-1:0]   s_mgmt_araddr,
    input  logic                    s_mgmt_arvalid,
    output logic                    s_mgmt_arready,
    output logic [DATA_WIDTH-1:0]   s_mgmt_rdata,
    output logic [1:0]              s_mgmt_rresp,
    output logic                    s_mgmt_rvalid,
    input  logic                    s_mgmt_rready,

    // =====================================================================
    // Sideband
    // =====================================================================
    input  logic                    decouple_i,
    output logic                    decouple_status_o,

    output logic                    evt_timeout_o,
    output logic                    evt_unsolicited_o,
    output logic                    irq_o
);

    // =====================================================================
    // Management Register Address Map (word index = addr[5:2])
    // =====================================================================
    localparam logic [3:0] REG_CTRL             = 4'h0;  // 0x00
    localparam logic [3:0] REG_STATUS           = 4'h1;  // 0x04
    localparam logic [3:0] REG_TIMEOUT_CYCLES   = 4'h2;  // 0x08
    localparam logic [3:0] REG_RESP_ON_TIMEOUT  = 4'h3;  // 0x0C
    localparam logic [3:0] REG_RDATA_ON_TIMEOUT = 4'h4;  // 0x10
    localparam logic [3:0] REG_TIMEOUT_COUNT    = 4'h5;  // 0x14
    localparam logic [3:0] REG_UNSOLICITED_COUNT= 4'h6;  // 0x18
    localparam logic [3:0] REG_MAX_OUTSTANDING  = 4'h7;  // 0x1C
    localparam logic [3:0] REG_IRQ_ENABLE       = 4'h8;  // 0x20

    // CTRL register bit positions
    localparam int CTRL_LOCKDOWN     = 0;
    localparam int CTRL_CLEAR_COUNTS = 1;
    localparam int CTRL_DECOUPLE     = 2;

    // STATUS register bit positions
    localparam int STATUS_LOCKED          = 0;
    localparam int STATUS_WR_OUTSTANDING  = 1;
    localparam int STATUS_RD_OUTSTANDING  = 2;
    localparam int STATUS_DECOUPLE        = 3;

    // IRQ_ENABLE bit positions
    localparam int IRQ_EN_TIMEOUT     = 0;
    localparam int IRQ_EN_UNSOLICITED = 1;

    // =====================================================================
    // Management Registers
    // =====================================================================

    // CTRL register fields
    logic        ctrl_lockdown_q;
    logic        ctrl_decouple_q;

    // Configuration registers
    logic [31:0] timeout_cycles_q;
    logic [1:0]  resp_on_timeout_q;
    logic [31:0] rdata_on_timeout_q;
    logic [31:0] max_outstanding_q;
    logic [31:0] timeout_count_q;
    logic [31:0] unsolicited_count_q;
    logic [31:0] irq_enable_q;

    // Derived
    assign decouple_status_o = decouple_i | ctrl_decouple_q;

    // =====================================================================
    // Write Channel Tracker
    // =====================================================================

    logic [3:0] wr_outstanding_q;
    logic [3:0] wr_phantom_q;

    // Timer FIFO for write channel (stores countdown per outstanding slot)
    logic [31:0] wr_timer_fifo [FIFO_DEPTH];
    logic [3:0]  wr_fifo_head_q, wr_fifo_tail_q;
    logic        wr_fifo_empty;
    logic        wr_fifo_full;

    assign wr_fifo_empty = (wr_fifo_head_q == wr_fifo_tail_q);
    assign wr_fifo_full  = ((wr_fifo_tail_q + 4'd1) == wr_fifo_head_q);

    // Write channel handshake tracking
    logic wr_aw_pending_q, wr_w_pending_q;

    // =====================================================================
    // Read Channel Tracker
    // =====================================================================

    logic [3:0] rd_outstanding_q;
    logic [3:0] rd_phantom_q;

    // Timer FIFO for read channel
    logic [31:0] rd_timer_fifo [FIFO_DEPTH];
    logic [3:0]  rd_fifo_head_q, rd_fifo_tail_q;
    logic        rd_fifo_empty;
    logic        rd_fifo_full;

    assign rd_fifo_empty = (rd_fifo_head_q == rd_fifo_tail_q);
    assign rd_fifo_full  = ((rd_fifo_tail_q + 4'd1) == rd_fifo_head_q);

    // =====================================================================
    // Upstream / Downstream Gating Logic
    // =====================================================================

    logic can_accept_wr;
    logic can_accept_rd;

    assign can_accept_wr = !ctrl_lockdown_q
                         && (wr_outstanding_q < max_outstanding_q[3:0])
                         && !wr_fifo_full;

    assign can_accept_rd = !ctrl_lockdown_q
                         && (rd_outstanding_q < max_outstanding_q[3:0])
                         && !rd_fifo_full;

    // --- Write Address Channel ---
    assign m_axi_awaddr  = s_axi_awaddr;
    assign m_axi_awprot  = s_axi_awprot;
    assign m_axi_awvalid = s_axi_awvalid && can_accept_wr && !wr_aw_pending_q;
    assign s_axi_awready = m_axi_awready && can_accept_wr && !wr_aw_pending_q;

    // --- Write Data Channel ---
    assign m_axi_wdata  = s_axi_wdata;
    assign m_axi_wstrb  = s_axi_wstrb;
    assign m_axi_wvalid = s_axi_wvalid && can_accept_wr && !wr_w_pending_q;
    assign s_axi_wready = m_axi_wready && can_accept_wr && !wr_w_pending_q;

    // --- Read Address Channel ---
    assign m_axi_araddr  = s_axi_araddr;
    assign m_axi_arprot  = s_axi_arprot;
    assign m_axi_arvalid = s_axi_arvalid && can_accept_rd;
    assign s_axi_arready = m_axi_arready && can_accept_rd;

    // =====================================================================
    // Write Response Channel (upstream B + downstream B)
    // =====================================================================

    // Timeout-generated synthetic write response
    logic        wr_timeout_pulse;
    logic        wr_synth_bvalid_q;
    logic [1:0]  wr_synth_bresp_q;

    // Downstream B: swallow if phantom > 0 or no outstanding
    logic wr_downstream_is_unsolicited;
    assign wr_downstream_is_unsolicited = m_axi_bvalid && (wr_phantom_q > 0 || wr_outstanding_q == 0);

    // Normal downstream B forwarding
    logic wr_forward_b;
    assign wr_forward_b = m_axi_bvalid && !wr_downstream_is_unsolicited && !wr_synth_bvalid_q;

    assign s_axi_bresp  = wr_synth_bvalid_q ? wr_synth_bresp_q : m_axi_bresp;
    assign s_axi_bvalid = wr_synth_bvalid_q || wr_forward_b;
    assign m_axi_bready = wr_downstream_is_unsolicited
                        || (wr_forward_b && s_axi_bready)
                        || (!m_axi_bvalid ? 1'b0 : 1'b0); // only when valid

    // =====================================================================
    // Read Response Channel (upstream R + downstream R)
    // =====================================================================

    logic        rd_timeout_pulse;
    logic        rd_synth_rvalid_q;
    logic [1:0]  rd_synth_rresp_q;
    logic [DATA_WIDTH-1:0] rd_synth_rdata_q;

    logic rd_downstream_is_unsolicited;
    assign rd_downstream_is_unsolicited = m_axi_rvalid && (rd_phantom_q > 0 || rd_outstanding_q == 0);

    logic rd_forward_r;
    assign rd_forward_r = m_axi_rvalid && !rd_downstream_is_unsolicited && !rd_synth_rvalid_q;

    assign s_axi_rdata  = rd_synth_rvalid_q ? rd_synth_rdata_q : m_axi_rdata;
    assign s_axi_rresp  = rd_synth_rvalid_q ? rd_synth_rresp_q : m_axi_rresp;
    assign s_axi_rvalid = rd_synth_rvalid_q || rd_forward_r;
    assign m_axi_rready = rd_downstream_is_unsolicited
                        || (rd_forward_r && s_axi_rready);

    // =====================================================================
    // Event Pulses & IRQ
    // =====================================================================

    logic evt_timeout_d, evt_unsolicited_d;

    assign evt_timeout_o     = evt_timeout_d;
    assign evt_unsolicited_o = evt_unsolicited_d;
    assign irq_o = (irq_enable_q[IRQ_EN_TIMEOUT] && evt_timeout_d)
                 | (irq_enable_q[IRQ_EN_UNSOLICITED] && evt_unsolicited_d);

    // =====================================================================
    // Main Sequential Logic
    // =====================================================================

    // Write channel: track AW and W acceptance to know when both phases complete
    logic wr_aw_accepted, wr_w_accepted;
    assign wr_aw_accepted = s_axi_awvalid && s_axi_awready;
    assign wr_w_accepted  = s_axi_wvalid  && s_axi_wready;

    // A write transaction is fully issued when both AW and W have been accepted
    logic wr_issued;
    assign wr_issued = (wr_aw_accepted || wr_aw_pending_q) && (wr_w_accepted || wr_w_pending_q);

    logic rd_issued;
    assign rd_issued = s_axi_arvalid && s_axi_arready;

    // Write B channel completion (upstream side)
    logic wr_b_completed;
    assign wr_b_completed = s_axi_bvalid && s_axi_bready && !wr_synth_bvalid_q;

    logic wr_synth_b_completed;
    assign wr_synth_b_completed = wr_synth_bvalid_q && s_axi_bready;

    // Read R channel completion (upstream side)
    logic rd_r_completed;
    assign rd_r_completed = s_axi_rvalid && s_axi_rready && !rd_synth_rvalid_q;

    logic rd_synth_r_completed;
    assign rd_synth_r_completed = rd_synth_rvalid_q && s_axi_rready;

    // Downstream unsolicited swallow events
    logic wr_unsol_swallow;
    assign wr_unsol_swallow = wr_downstream_is_unsolicited && m_axi_bvalid;

    logic rd_unsol_swallow;
    assign rd_unsol_swallow = rd_downstream_is_unsolicited && m_axi_rvalid;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            wr_outstanding_q   <= 4'd0;
            wr_phantom_q       <= 4'd0;
            wr_fifo_head_q     <= 4'd0;
            wr_fifo_tail_q     <= 4'd0;
            wr_aw_pending_q    <= 1'b0;
            wr_w_pending_q     <= 1'b0;
            wr_synth_bvalid_q  <= 1'b0;
            wr_synth_bresp_q   <= 2'b00;

            rd_outstanding_q   <= 4'd0;
            rd_phantom_q       <= 4'd0;
            rd_fifo_head_q     <= 4'd0;
            rd_fifo_tail_q     <= 4'd0;
            rd_synth_rvalid_q  <= 1'b0;
            rd_synth_rresp_q   <= 2'b00;
            rd_synth_rdata_q   <= '0;

            timeout_count_q    <= 32'd0;
            unsolicited_count_q <= 32'd0;

            evt_timeout_d      <= 1'b0;
            evt_unsolicited_d  <= 1'b0;
        end else begin
            evt_timeout_d     <= 1'b0;
            evt_unsolicited_d <= 1'b0;

            // ---------------------------------------------------------
            // Write AW/W pending tracking
            // ---------------------------------------------------------
            if (wr_issued) begin
                wr_aw_pending_q <= 1'b0;
                wr_w_pending_q  <= 1'b0;
            end else begin
                if (wr_aw_accepted) wr_aw_pending_q <= 1'b1;
                if (wr_w_accepted)  wr_w_pending_q  <= 1'b1;
            end

            // ---------------------------------------------------------
            // Write outstanding counter & timer FIFO push
            // ---------------------------------------------------------
            if (wr_issued) begin
                wr_outstanding_q <= wr_outstanding_q + 4'd1;
                wr_timer_fifo[wr_fifo_tail_q] <= timeout_cycles_q;
                wr_fifo_tail_q <= wr_fifo_tail_q + 4'd1;
            end

            // ---------------------------------------------------------
            // Write timer FIFO tick & timeout detection
            // ---------------------------------------------------------
            wr_timeout_pulse <= 1'b0;
            if (!wr_fifo_empty) begin
                if (wr_timer_fifo[wr_fifo_head_q] == 32'd0) begin
                    // Timeout!
                    wr_timeout_pulse <= 1'b1;
                    wr_fifo_head_q   <= wr_fifo_head_q + 4'd1;
                    wr_phantom_q     <= wr_phantom_q + 4'd1;
                    // Generate synthetic B response
                    wr_synth_bvalid_q <= 1'b1;
                    wr_synth_bresp_q  <= resp_on_timeout_q;
                    timeout_count_q   <= timeout_count_q + 32'd1;
                    evt_timeout_d     <= 1'b1;
                end else begin
                    wr_timer_fifo[wr_fifo_head_q] <= wr_timer_fifo[wr_fifo_head_q] - 32'd1;
                end
            end

            // Synthetic B response handshake — also decrement outstanding
            if (wr_synth_b_completed) begin
                wr_synth_bvalid_q <= 1'b0;
                wr_outstanding_q  <= wr_outstanding_q - 4'd1;
            end

            // Normal write B completion — decrement outstanding
            if (wr_b_completed) begin
                wr_outstanding_q <= wr_outstanding_q - 4'd1;
                wr_fifo_head_q   <= wr_fifo_head_q + 4'd1;
            end

            // Unsolicited write B swallow — decrement phantom
            if (wr_unsol_swallow) begin
                if (wr_phantom_q > 0) begin
                    wr_phantom_q <= wr_phantom_q - 4'd1;
                end
                unsolicited_count_q <= unsolicited_count_q + 32'd1;
                evt_unsolicited_d   <= 1'b1;
            end

            // ---------------------------------------------------------
            // Read outstanding counter & timer FIFO push
            // ---------------------------------------------------------
            if (rd_issued) begin
                rd_outstanding_q <= rd_outstanding_q + 4'd1;
                rd_timer_fifo[rd_fifo_tail_q] <= timeout_cycles_q;
                rd_fifo_tail_q <= rd_fifo_tail_q + 4'd1;
            end

            // ---------------------------------------------------------
            // Read timer FIFO tick & timeout detection
            // ---------------------------------------------------------
            rd_timeout_pulse <= 1'b0;
            if (!rd_fifo_empty) begin
                if (rd_timer_fifo[rd_fifo_head_q] == 32'd0) begin
                    // Timeout!
                    rd_timeout_pulse <= 1'b1;
                    rd_fifo_head_q   <= rd_fifo_head_q + 4'd1;
                    rd_phantom_q     <= rd_phantom_q + 4'd1;
                    // Generate synthetic R response
                    rd_synth_rvalid_q <= 1'b1;
                    rd_synth_rresp_q  <= resp_on_timeout_q;
                    rd_synth_rdata_q  <= rdata_on_timeout_q;
                    timeout_count_q   <= timeout_count_q + 32'd1;
                    evt_timeout_d     <= 1'b1;
                end else begin
                    rd_timer_fifo[rd_fifo_head_q] <= rd_timer_fifo[rd_fifo_head_q] - 32'd1;
                end
            end

            // Synthetic R response handshake — also decrement outstanding
            if (rd_synth_r_completed) begin
                rd_synth_rvalid_q <= 1'b0;
                rd_outstanding_q  <= rd_outstanding_q - 4'd1;
            end

            // Normal read R completion — decrement outstanding
            if (rd_r_completed) begin
                rd_outstanding_q <= rd_outstanding_q - 4'd1;
                rd_fifo_head_q   <= rd_fifo_head_q + 4'd1;
            end

            // Unsolicited read R swallow — decrement phantom
            if (rd_unsol_swallow) begin
                if (rd_phantom_q > 0) begin
                    rd_phantom_q <= rd_phantom_q - 4'd1;
                end
                unsolicited_count_q <= unsolicited_count_q + 32'd1;
                evt_unsolicited_d   <= 1'b1;
            end

            // Adjust outstanding for simultaneous issue and timeout/complete
            // (handled by the individual +1/-1 above; no extra correction needed
            // because timeout removes from FIFO head while issue adds to tail)
        end
    end

    // =====================================================================
    // Management Register File (s_mgmt AXI-Lite)
    // =====================================================================

    // Read channel state
    logic [7:0]  mgmt_rd_addr_q;
    logic        mgmt_rd_pending_q;
    logic        mgmt_rvalid_q;
    logic [31:0] mgmt_rdata_q;

    // Write channel state
    logic [7:0]  mgmt_wr_addr_q;
    logic [31:0] mgmt_wr_data_q;
    logic        mgmt_wr_addr_valid_q;
    logic        mgmt_wr_data_valid_q;
    logic        mgmt_bvalid_q;

    assign s_mgmt_arready = 1'b1;
    assign s_mgmt_rdata   = mgmt_rdata_q;
    assign s_mgmt_rresp   = 2'b00;
    assign s_mgmt_rvalid  = mgmt_rvalid_q;

    assign s_mgmt_awready = 1'b1;
    assign s_mgmt_wready  = 1'b1;
    assign s_mgmt_bresp   = 2'b00;
    assign s_mgmt_bvalid  = mgmt_bvalid_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            ctrl_lockdown_q    <= 1'b0;
            ctrl_decouple_q    <= 1'b0;
            timeout_cycles_q   <= TIMEOUT_CYCLES_INIT;
            resp_on_timeout_q  <= RESP_ON_TIMEOUT_INIT;
            rdata_on_timeout_q <= RDATA_ON_TIMEOUT_INIT;
            max_outstanding_q  <= MAX_OUTSTANDING_INIT;
            irq_enable_q       <= 32'd0;

            mgmt_rd_addr_q       <= 8'd0;
            mgmt_rd_pending_q    <= 1'b0;
            mgmt_rvalid_q        <= 1'b0;
            mgmt_rdata_q         <= 32'd0;
            mgmt_wr_addr_q       <= 8'd0;
            mgmt_wr_data_q       <= 32'd0;
            mgmt_wr_addr_valid_q <= 1'b0;
            mgmt_wr_data_valid_q <= 1'b0;
            mgmt_bvalid_q        <= 1'b0;
        end else begin
            // =============================================================
            // Read path
            // =============================================================
            if (s_mgmt_arvalid && s_mgmt_arready) begin
                mgmt_rd_addr_q    <= s_mgmt_araddr[7:0];
                mgmt_rd_pending_q <= 1'b1;
            end

            if (mgmt_rd_pending_q && !mgmt_rvalid_q) begin
                mgmt_rvalid_q     <= 1'b1;
                mgmt_rd_pending_q <= 1'b0;

                case (mgmt_rd_addr_q[5:2])
                    REG_CTRL: begin
                        mgmt_rdata_q <= 32'd0;
                        mgmt_rdata_q[CTRL_LOCKDOWN] <= ctrl_lockdown_q;
                        mgmt_rdata_q[CTRL_DECOUPLE] <= ctrl_decouple_q;
                    end
                    REG_STATUS: begin
                        mgmt_rdata_q <= 32'd0;
                        mgmt_rdata_q[STATUS_LOCKED]         <= ctrl_lockdown_q;
                        mgmt_rdata_q[STATUS_WR_OUTSTANDING] <= (wr_outstanding_q != 4'd0);
                        mgmt_rdata_q[STATUS_RD_OUTSTANDING] <= (rd_outstanding_q != 4'd0);
                        mgmt_rdata_q[STATUS_DECOUPLE]       <= decouple_status_o;
                    end
                    REG_TIMEOUT_CYCLES:    mgmt_rdata_q <= timeout_cycles_q;
                    REG_RESP_ON_TIMEOUT:   mgmt_rdata_q <= {30'd0, resp_on_timeout_q};
                    REG_RDATA_ON_TIMEOUT:  mgmt_rdata_q <= rdata_on_timeout_q;
                    REG_TIMEOUT_COUNT:     mgmt_rdata_q <= timeout_count_q;
                    REG_UNSOLICITED_COUNT: mgmt_rdata_q <= unsolicited_count_q;
                    REG_MAX_OUTSTANDING:   mgmt_rdata_q <= max_outstanding_q;
                    REG_IRQ_ENABLE:        mgmt_rdata_q <= irq_enable_q;
                    default:               mgmt_rdata_q <= 32'hDEADBEEF;
                endcase
            end

            if (mgmt_rvalid_q && s_mgmt_rready) begin
                mgmt_rvalid_q <= 1'b0;
            end

            // =============================================================
            // Write path
            // =============================================================
            if (s_mgmt_awvalid && s_mgmt_awready) begin
                mgmt_wr_addr_q       <= s_mgmt_awaddr[7:0];
                mgmt_wr_addr_valid_q <= 1'b1;
            end

            if (s_mgmt_wvalid && s_mgmt_wready) begin
                mgmt_wr_data_q       <= s_mgmt_wdata;
                mgmt_wr_data_valid_q <= 1'b1;
            end

            if (mgmt_wr_addr_valid_q && mgmt_wr_data_valid_q && !mgmt_bvalid_q) begin
                mgmt_wr_addr_valid_q <= 1'b0;
                mgmt_wr_data_valid_q <= 1'b0;
                mgmt_bvalid_q        <= 1'b1;

                case (mgmt_wr_addr_q[5:2])
                    REG_CTRL: begin
                        ctrl_lockdown_q <= mgmt_wr_data_q[CTRL_LOCKDOWN];
                        if (mgmt_wr_data_q[CTRL_CLEAR_COUNTS]) begin
                            timeout_count_q     <= 32'd0;
                            unsolicited_count_q <= 32'd0;
                        end
                        ctrl_decouple_q <= mgmt_wr_data_q[CTRL_DECOUPLE];
                    end
                    REG_TIMEOUT_CYCLES:    timeout_cycles_q   <= mgmt_wr_data_q;
                    REG_RESP_ON_TIMEOUT:   resp_on_timeout_q  <= mgmt_wr_data_q[1:0];
                    REG_RDATA_ON_TIMEOUT:  rdata_on_timeout_q <= mgmt_wr_data_q;
                    REG_MAX_OUTSTANDING:   max_outstanding_q  <= mgmt_wr_data_q;
                    REG_IRQ_ENABLE:        irq_enable_q       <= mgmt_wr_data_q;
                    default: ;
                endcase
            end

            if (mgmt_bvalid_q && s_mgmt_bready) begin
                mgmt_bvalid_q <= 1'b0;
            end
        end
    end

endmodule
