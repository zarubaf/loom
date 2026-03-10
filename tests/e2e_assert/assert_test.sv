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
    // Immediate assertions
    // ================================================================

    // I1: Combinational — state must never be StError.  FIRES at cycle ~21.
    always_comb begin
        i1_no_error: assert (state_q != StError);
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
