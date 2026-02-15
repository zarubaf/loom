// SPDX-License-Identifier: Apache-2.0
// dpi_example.sv - Test module with DPI-C function calls
// Used to test DPI bridge pass

// DPI-C function imports
import "DPI-C" function int dpi_add(input int a, input int b);
import "DPI-C" function void dpi_log(input string msg);
import "DPI-C" context function int dpi_read_mem(input int addr);
import "DPI-C" context function void dpi_write_mem(input int addr, input int data);

module dpi_example (
    input  logic        clk,
    input  logic        rst,
    input  logic [31:0] a,
    input  logic [31:0] b,
    input  logic        start,
    output logic [31:0] result,
    output logic        done
);
    typedef enum logic [1:0] {
        IDLE,
        COMPUTE,
        DONE
    } state_t;

    state_t state;
    logic [31:0] sum;

    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            state <= IDLE;
            sum <= '0;
            done <= 1'b0;
        end else begin
            case (state)
                IDLE: begin
                    done <= 1'b0;
                    if (start) begin
                        // Call DPI function to compute sum
                        sum <= dpi_add(a, b);
                        state <= COMPUTE;
                    end
                end
                COMPUTE: begin
                    // Could call dpi_log here in simulation
                    state <= DONE;
                end
                DONE: begin
                    done <= 1'b1;
                    state <= IDLE;
                end
            endcase
        end
    end

    assign result = sum;
endmodule
