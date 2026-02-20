// SPDX-License-Identifier: Apache-2.0
// Error test: DPI call with used result in initial block
// This should produce an error from the Loom frontend.

import "DPI-C" function int get_value();

module initial_dpi_error_test (
    input  logic clk_i,
    input  logic rst_ni
);
    logic [31:0] x;
    initial begin
        x = get_value();  // Should produce error
    end
endmodule
