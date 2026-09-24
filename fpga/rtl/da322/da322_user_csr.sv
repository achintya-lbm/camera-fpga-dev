// DA322 user control/status registers, APB slave behind Hololink user window 6 (0x7000_0000):
// the register layout Tauro's vendor bitstream exposes, so hsb/board/da322 works unchanged.
//
//   0x0  USER_CSR      bit0 ST_CLEAR (RW): while 1, the latched MIPI_DT_STAT values are cleared
//   0x4  MIPI_DT_CTRL  [7:0] camera 0 (the one-camera image uses byte 0 only): forward only this CSI data type; 0 = all
//   0x8  MIPI_DT_STAT  same layout, RO: last long-packet data type seen per camera (0x00/0x01 excluded)
//   0xC  BUILD_ID      RO: 0xDA32_2xxx identifies this design (not present in the vendor image)
module da322_user_csr #(
  parameter [31:0] BUILD_ID = 32'hDA32_2001
)(
  input               i_apb_clk,
  input               i_apb_rst,      // synchronous active high
  input               i_apb_sel,
  input               i_apb_enable,
  input      [31:0]   i_apb_addr,
  input      [31:0]   i_apb_wdata,
  input               i_apb_write,
  output reg          o_apb_ready,
  output reg [31:0]   o_apb_rdata,
  output              o_apb_serr,

  output     [31:0]   o_dt_filter,    // {cam3, cam2, cam1, cam0}
  output              o_dt_seen_clear,
  input      [31:0]   i_dt_seen       // {cam3, cam2, cam1, cam0}, latched in the receivers' byte clocks
);

  reg        st_clear;
  reg [31:0] dt_filter;
  // 2-flop resync of the slowly changing status word (each byte changes at most once per line).
  reg [31:0] dt_seen_meta, dt_seen_sync;

  assign o_apb_serr      = 1'b0;
  assign o_dt_filter     = dt_filter;
  assign o_dt_seen_clear = st_clear;

  always @(posedge i_apb_clk) begin
    if (i_apb_rst) begin
      st_clear     <= 1'b0;
      dt_filter    <= 32'h0;
      o_apb_ready  <= 1'b0;
      o_apb_rdata  <= 32'h0;
      dt_seen_meta <= 32'h0;
      dt_seen_sync <= 32'h0;
    end else begin
      dt_seen_meta <= i_dt_seen;
      dt_seen_sync <= dt_seen_meta;
      o_apb_ready  <= i_apb_sel && i_apb_enable && !o_apb_ready;
      if (i_apb_sel && i_apb_enable && !o_apb_ready) begin
        if (i_apb_write) begin
          case (i_apb_addr[3:2])
            2'd0: st_clear  <= i_apb_wdata[0];
            2'd1: dt_filter <= i_apb_wdata;
            default: ;
          endcase
        end
        case (i_apb_addr[3:2])
          2'd0: o_apb_rdata <= {31'h0, st_clear};
          2'd1: o_apb_rdata <= dt_filter;
          2'd2: o_apb_rdata <= dt_seen_sync;
          default: o_apb_rdata <= BUILD_ID;
        endcase
      end
    end
  end

endmodule
