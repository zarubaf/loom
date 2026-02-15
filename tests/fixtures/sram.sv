// SPDX-License-Identifier: Apache-2.0

/// Generic behaviorial RAM model
///
/// This is a simple generic behavorial model for a Random Access Memory (RAM).
/// This version is iverilog-compatible (uses unpacked arrays and simpler syntax).
module sram #(
  /// Depth of the RAM (i.e., the number of words)
  parameter  int unsigned Depth       = 128,
  /// Bit width of a data word
  parameter  int unsigned DataWidth   = 128,
  /// Bit width of an individual byte of data (used for byte enable masking)
  parameter  int unsigned ByteWidth   = 8,
  /// Implementation key, may be used by synthesis tools to lookup additional
  /// parameters for implementation.
  parameter               ImplKey     = "none",
  /// Bit width of RAM addresses
  localparam int unsigned AddrWidth   = $clog2(Depth),
  /// Bit width of byte enable mask
  localparam int unsigned ByteEnWidth = (DataWidth + ByteWidth - 32'd1) / ByteWidth
) (
  input  logic                   clk_i,
  /// Request active signal (mapped to the chip-select pin of SRAM macros)
  input  logic                   req_i,
  /// Write-enable signal
  input  logic                   we_i,
  /// Read/write address
  input  logic [AddrWidth  -1:0] addr_i,
  /// Byte-enable mask
  input  logic [ByteEnWidth-1:0] be_i,
  /// Write data
  input  logic [DataWidth  -1:0] wdata_i,
  /// Read data (only valid after req_i was asserted and we_i de-asserted)
  output logic [DataWidth  -1:0] rdata_o1
);

  // Unpacked array for iverilog compatibility
  reg [DataWidth-1:0] mem [0:Depth-1];

  // Write logic with byte enables
  integer b;
  always @(posedge clk_i) begin
    if (req_i && we_i) begin
      for (b = 0; b < ByteEnWidth; b = b + 1) begin
        if (be_i[b]) begin
          mem[addr_i][b * ByteWidth +: ByteWidth] <= wdata_i[b * ByteWidth +: ByteWidth];
        end
      end
    end
  end

  // Read logic (registered output)
  always @(posedge clk_i) begin
    if (req_i && !we_i) begin
      rdata_o1 <= mem[addr_i];
    end
  end

endmodule
