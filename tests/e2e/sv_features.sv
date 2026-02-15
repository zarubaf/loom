// sv_features.sv - SystemVerilog test with structs and enums
// Tests that yosys-slang correctly handles SV-specific features

typedef enum logic [1:0] {
    IDLE   = 2'b00,
    LOAD   = 2'b01,
    COMPUTE = 2'b10,
    DONE   = 2'b11
} state_t;

typedef struct packed {
    logic [7:0] data;
    logic       valid;
    logic       last;
} packet_t;

module sv_features (
    input  logic    clk,
    input  logic    rst,
    input  logic    start,
    input  packet_t pkt_in,
    output packet_t pkt_out,
    output state_t  state
);
    state_t  current_state;
    packet_t stored_pkt;

    always_ff @(posedge clk or posedge rst) begin
        if (rst) begin
            current_state <= IDLE;
            stored_pkt <= '0;
        end else begin
            case (current_state)
                IDLE: begin
                    if (start) begin
                        current_state <= LOAD;
                    end
                end
                LOAD: begin
                    stored_pkt <= pkt_in;
                    current_state <= COMPUTE;
                end
                COMPUTE: begin
                    stored_pkt.data <= stored_pkt.data + 8'd1;
                    current_state <= DONE;
                end
                DONE: begin
                    current_state <= IDLE;
                end
            endcase
        end
    end

    assign pkt_out = stored_pkt;
    assign state = current_state;
endmodule
