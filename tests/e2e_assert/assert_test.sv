// SPDX-License-Identifier: Apache-2.0
// assert_test — Synthesizable assertion e2e test
//
// A counter-based DUT with both immediate and concurrent (SVA) assertions.
// After ~21 cycles the state machine deliberately enters an illegal state,
// triggering assertion failures that should print a message and halt
// emulation via loom_finish_o.

module assert_test (
    input  logic        clk_i,
    input  logic        rst_ni
);

    // ----------------------------------------------------------------
    // 8-bit free-running counter
    // ----------------------------------------------------------------
    logic [7:0] cnt_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) cnt_q <= 8'd0;
        else         cnt_q <= cnt_q + 8'd1;
    end

    // ----------------------------------------------------------------
    // State machine — enters illegal StError at cycle ~21
    // ----------------------------------------------------------------
    typedef enum logic [1:0] {
        StIdle   = 2'b00,
        StActive = 2'b01,
        StDone   = 2'b10,
        StError  = 2'b11
    } state_e;

    state_e state_q, state_d;

    always_comb begin
        state_d = state_q;
        unique case (state_q)
            StIdle:   if (cnt_q == 8'd5)  state_d = StActive;
            StActive: if (cnt_q == 8'd15) state_d = StDone;
            StDone:   if (cnt_q == 8'd20) state_d = StError; // deliberate bug
            StError:  state_d = StError;
            default:  state_d = StIdle;
        endcase
    end

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) state_q <= StIdle;
        else         state_q <= state_d;
    end

    // ----------------------------------------------------------------
    // FIFO write pointer (active during StActive)
    // ----------------------------------------------------------------
    logic [3:0] wptr_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)                  wptr_q <= 4'd0;
        else if (state_q == StActive) wptr_q <= wptr_q + 4'd1;
    end

    // Previous counter for monotonicity check
    logic [7:0] cnt_prev_q;
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) cnt_prev_q <= 8'd0;
        else         cnt_prev_q <= cnt_q;
    end

    // ================================================================
    // Concurrent assertions (SVA — assert property)
    // ================================================================

    // C1: State must never be StError — FIRES at cycle ~21
    c1_no_error: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        state_q != StError
    );

    // C2: Counter monotonicity (always passes)
    c2_cnt_mono: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        cnt_q == 8'd0 || cnt_q == cnt_prev_q + 8'd1
    );

    // C3: FIFO pointer stays below 12 during active (always passes)
    c3_wptr_bound: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        !(state_q == StActive) || wptr_q < 4'd12
    );

    // C4: When Done, counter >= 15 (always passes)
    c4_done_cnt: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        !(state_q == StDone) || cnt_q >= 8'd15
    );

    // C5: Assume — should be silently lowered (no emulation effect)
    c5_rst_clean: assume property (
        @(posedge clk_i)
        rst_ni !== 1'bx
    );

    // ================================================================
    // Sequence assertions (SVA — ##N delays, implications)
    // ================================================================

    // S1: When counter is 5 (Idle->Active transition), state must be Active
    //     one cycle later. (always passes)
    s1_idle_to_active: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        (cnt_q == 8'd5) |-> ##1 (state_q == StActive)
    );

    // S2: Overlapping implication — when Active, counter must be non-zero.
    //     (always passes since Active starts at cnt=6)
    s2_active_cnt: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        (state_q == StActive) |-> (cnt_q > 8'd0)
    );

    // S3: When counter is 15 (Active->Done), state must be Done two
    //     cycles later. (always passes)
    s3_active_to_done: assert property (
        @(posedge clk_i) disable iff (!rst_ni)
        (cnt_q == 8'd15) |-> ##2 (state_q == StDone)
    );

    // S4: $rose/$fell — detect state transitions.
    //     When reset deasserts ($rose(rst_ni)), state must be Idle.
    //     (always passes)
    always_ff @(posedge clk_i) begin
        if ($rose(rst_ni))
            s4_rst_deassert: assert (state_q == StIdle);
    end

    // S5: $stable — counter previous value stability check.
    //     When counter is 0, cnt_prev_q must be stable (both 0 after reset).
    //     This trivially passes at the first cycle after reset.
    always_ff @(posedge clk_i) begin
        if (rst_ni && cnt_q == 8'd1)
            s5_prev_stable: assert ($past(cnt_prev_q) == 8'd0);
    end

    // ================================================================
    // Immediate assertions
    // ================================================================

    // I1: Combinational — state must never be StError.  FIRES at cycle ~21.
    //     Has an else clause with $error message.
    always_comb begin
        i1_no_error: assert (state_q != StError)
            else $error("I1: illegal state StError reached!");
    end

    // I2: Clocked — counter monotonicity (always passes)
    always_ff @(posedge clk_i) begin
        if (rst_ni && cnt_q != 8'd0)
            i2_cnt_mono: assert (cnt_q == cnt_prev_q + 8'd1);
    end

    // I3: Clocked — FIFO pointer < 12 during active (always passes)
    always_ff @(posedge clk_i) begin
        if (rst_ni && state_q == StActive)
            i3_wptr_bound: assert (wptr_q < 4'd12);
    end

    // I4: Combinational — when Done, counter >= 15 (always passes)
    always_comb begin
        if (state_q == StDone)
            i4_done_cnt: assert (cnt_q >= 8'd15);
    end

    // ================================================================
    // $display progress markers (existing infrastructure)
    // ================================================================
    always_ff @(posedge clk_i) begin
        if (cnt_q == 8'd1)
            $display("[assert_test] started, cnt=%0d", cnt_q);
        if (state_q == StActive && cnt_q == 8'd10)
            $display("[assert_test] active, wptr=%0d", wptr_q);
    end

endmodule
