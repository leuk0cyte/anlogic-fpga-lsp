module mod_case (input logic [1:0] sel, output logic y);
  always_comb begin
    case (sel)
      2'd0: y = 1'b0;
      2'd1: y = 1'b1;
      default: y = 1'b0;
  end
endmodule
