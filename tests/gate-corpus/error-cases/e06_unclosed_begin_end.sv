module mod_begin (input logic clk);
  always @(posedge clk) begin
    if (clk) begin
      x <= 1'b1;
  end
endmodule
