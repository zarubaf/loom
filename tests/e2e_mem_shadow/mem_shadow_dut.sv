// SPDX-License-Identifier: Apache-2.0
// E2E test DUT for mem_shadow integration
//
// Tests memory shadow functionality with:
//   1. Top-level RAM with initial assignments (8-bit x 16)
//   2. Submodule ROM with initial assignments (8-bit x 8, different hierarchy level)
//   3. A DPI call (dpi_checksum) to verify the full pipeline works alongside memory shadow
//   4. Async-reset counter and state machine (so reset_extract + scan_insert have work)
//
// The DUT writes to ram[counter[3:0]] on each cycle, which lets us verify:
//   - Initial memory content is preloaded correctly via shadow ports
//   - Host can observe memory writes via shadow read (dump)
//   - DPI calls work alongside memory shadow
//   - Reset re-preloads memories to initial state

import "DPI-C" function int dpi_checksum(input int a, input int b);

// Submodule: ROM loaded via $readmemh (hierarchy test)
module rom_sub (
    input  logic        clk_i,
    input  logic [2:0]  addr_i,
    output logic [7:0]  data_o
);
    logic [7:0] rom [0:7];

    initial $readmemh("rom_data.hex", rom);

    always_ff @(posedge clk_i) begin
        data_o <= rom[addr_i];
    end
endmodule

// Submodule: LUT loaded via $readmemb (tests binary file loading)
module lut_sub (
    input  logic        clk_i,
    input  logic [1:0]  addr_i,
    output logic [7:0]  data_o
);
    logic [7:0] lut [0:3];

    initial $readmemb("lut_data.bin", lut);

    always_ff @(posedge clk_i) begin
        data_o <= lut[addr_i];
    end
endmodule

// Top-level module
module mem_shadow_dut (
    input  logic        clk_i,
    input  logic        rst_ni,
    output logic [7:0]  rom_data_o,
    output logic [7:0]  lut_data_o,
    output logic [15:0] counter_o,
    output logic [31:0] checksum_o
);

    // =========================================================================
    // Top-level RAM with initial assignments (8-bit x 16)
    // =========================================================================
    logic [7:0] ram [0:15];

    // Known initial values — the host verifies these after preload
    initial begin
        ram[0]  = 8'h10;
        ram[1]  = 8'h20;
        ram[2]  = 8'h30;
        ram[3]  = 8'h40;
        ram[4]  = 8'h50;
        ram[5]  = 8'h60;
        ram[6]  = 8'h70;
        ram[7]  = 8'h80;
        ram[8]  = 8'h90;
        ram[9]  = 8'hA0;
        ram[10] = 8'hB0;
        ram[11] = 8'hC0;
        ram[12] = 8'hD0;
        ram[13] = 8'hE0;
        ram[14] = 8'hF0;
        ram[15] = 8'hFF;
    end

    // The DUT writes to ram[counter_q[3:0]] on each cycle it runs
    logic [7:0]  ram_rdata;
    logic [15:0] counter_q;

    always_ff @(posedge clk_i) begin
        ram[counter_q[3:0]] <= counter_q[7:0];
        ram_rdata <= ram[counter_q[3:0]];
    end

    // =========================================================================
    // Counter with async reset (for reset_extract)
    // =========================================================================
    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni)
            counter_q <= 16'h0001;
        else
            counter_q <= counter_q + 16'd1;
    end

    assign counter_o = counter_q;

    // =========================================================================
    // State machine with DPI call
    // =========================================================================
    typedef enum logic [2:0] {
        StIdle,
        StCompute,
        StDone
    } state_e;

    state_e      state_q;
    logic [31:0] checksum_q;

    always_ff @(posedge clk_i or negedge rst_ni) begin
        if (!rst_ni) begin
            state_q    <= StIdle;
            checksum_q <= 32'd0;
        end else begin
            case (state_q)
                StIdle: begin
                    checksum_q <= dpi_checksum({24'd0, ram_rdata}, 32'hAA);
                    state_q    <= StCompute;
                end
                StCompute: begin
                    state_q <= StDone;
                end
                StDone: begin
                    // Stay here; counter keeps incrementing
                end
            endcase
        end
    end

    assign checksum_o = checksum_q;

    // =========================================================================
    // Submodule ROM instance (hierarchy test — $readmemh)
    // =========================================================================
    rom_sub u_rom (
        .clk_i  (clk_i),
        .addr_i (counter_q[2:0]),
        .data_o (rom_data_o)
    );

    // =========================================================================
    // Submodule LUT instance ($readmemb test)
    // =========================================================================
    lut_sub u_lut (
        .clk_i  (clk_i),
        .addr_i (counter_q[1:0]),
        .data_o (lut_data_o)
    );

endmodule
