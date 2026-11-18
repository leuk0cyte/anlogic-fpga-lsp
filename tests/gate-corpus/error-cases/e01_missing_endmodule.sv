module mod_a (
  input  logic clk,
  input  logic rst_n
);
  always_ff @(posedge clk) begin
    if (!rst_n) q <= 1'b0;
  end
endmodule
// intent: module mod_b is never closed
module mod_b (
  input logic a
);
  assign b = a;
