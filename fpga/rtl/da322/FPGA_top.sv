// Tauro DA322 one-camera top level: CAM4 (connector J1D) -> Hololink IP -> 10G SFP+.
//
// Derived from holoscan-sensor-bridge 2.7.0 fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/rtl/top/FPGA_top.sv
// (Apache-2.0, NVIDIA), whose camera 0 already sits on the DA322's J1D balls. Kept deliberately minimal:
//   * one camera receiver with Tauro's CSI data-type filter, user window 2 (0x3000_0000; D-PHY lane
//     register at 0x3000_0028 as in the vendor image, camera index 0)
//   * two I2C buses: 0 = control/EEPROM (H7/H6), 1 = J1D camera bus (D15/D16)
//   * CAM_EN of J1D driven by Hololink GPIO 0; CAM_MCLK of J1D driven with 27.043 MHz
//   * USER_CSR / MIPI_DT_CTRL / MIPI_DT_STAT / BUILD_ID at 0x7000_000x
//   * no MFP GPIO, no deserializer/PoC/VSYNC logic (DA326-only), no other connectors
// The four-camera variant is in git history (branch fpga-da322, commit 2f4cda8).
`include "HOLOLINK_def.svh"

module FPGA_top
  import HOLOLINK_pkg::*;
  import apb_pkg::*;
#(
  parameter BUILD_REV = 48'h0
)(
  input           RESET_N,                 // H1, pulled up
  // 10GbE SFP+
  input           ETH_REFCLK_P,            // D10 / E10, 161.1328125 MHz
  input           ETH_REFCLK_N,
  input           ETH_RXD_P,
  input           ETH_RXD_N,
  output          ETH_TXD_P,
  output          ETH_TXD_N,
  output          SFP_TX_DIS,

  // MIPI CSI-2, J1D
  inout           MIPI_CAM_CLK_P,
  inout           MIPI_CAM_CLK_N,
  inout   [3:0]   MIPI_CAM_DATA_P,
  inout   [3:0]   MIPI_CAM_DATA_N,

  // I2C
  inout           CTRL_I2C_SCL,
  inout           CTRL_I2C_SDA,
  inout           CAM_I2C_SCL,
  inout           CAM_I2C_SDA,

  // J1D pin 17 (camera enable) and pin 18 (reference clock)
  output          CAM_EN,
  output          CAM_MCLK,

  // QSPI configuration flash
  output          FLASH_SPI_MCSN,
  output          FLASH_SPI_MSCK,
  inout   [3:0]   FLASH_SPI_SDIO
);

//------------------------------------------------------------------------------
// Clock and Reset
//------------------------------------------------------------------------------

  logic [`SENSOR_RX_IF_INST-1:0] sw_sen_rst;
  logic                          sw_sys_rst;

  logic [`HOST_IF_INST  -1:0] usr_clk;     // pcs user clock out
  logic [`HOST_IF_INST  -1:0] usr_clk_rdy; // pcs user clock out ready
  logic                       usr_clk_locked;
  logic                       mipi_clk;    // 27.043 MHz camera reference clock
  /* synthesis syn_keep=1 nomerge=""*/
  logic                       pcs_clk;     // 100-300 MHz PCS calibration clock
  /* synthesis syn_keep=1 nomerge=""*/
  logic                       apb_clk;     // ctrl plane clock
  /* synthesis syn_keep=1 nomerge=""*/
  logic                       hif_clk;     // data plane clock
  logic                       sys_rst;     // system active high reset
  logic                       apb_rst;     // apb active high reset
  logic                       hif_rst;     // host interface active high reset
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_rst;  // sensor interface active high reset
  logic                       pcs_rst_n;   // ethernet pcs active low reset
  /* synthesis syn_keep=1 nomerge=""*/
  logic        ptp_clk;
  logic        ptp_rst;
  logic [31:0] ptp_nsec;
  logic [47:0] ptp_sec;
  logic        ptp_cam_clk;
  logic [15:0] gpio_out;
  logic [15:0] gpio_dir;
  logic [15:0] gpio_in;
  logic        sys_pps;

  assign usr_clk_locked = &usr_clk_rdy;

  clk_n_rst u_clk_n_rst (
    .i_refclk      ( usr_clk [0]    ),
    .i_locked      ( usr_clk_locked ),
    .o_mipi_clk    ( mipi_clk       ),
    .o_pcs_clk     ( pcs_clk        ),
    .o_hif_clk     ( hif_clk        ),
    .o_apb_clk     ( apb_clk        ),
    .o_ptp_clk     ( ptp_clk        ),
    .i_ptp_nsec    ( ptp_nsec       ),
    .o_ptp_cam_clk ( ptp_cam_clk    ),
    .i_pb_rst_n    ( RESET_N        ),
    .i_sw_rst      ( sw_sys_rst     ),
    .o_sys_rst     ( sys_rst        ),
    .o_pcs_rst_n   ( pcs_rst_n      )
  );

//------------------------------------------------------------------------------
// Board control
//------------------------------------------------------------------------------

  assign SFP_TX_DIS = 1'b0;

  // Camera enable = Hololink GPIO 0 (the host drives GPIO k high to power camera k).
  assign CAM_EN      = gpio_out[0];
  assign gpio_in     = {15'h0, gpio_out[0]};

  // Camera reference clock on J1D pin 18 (the FSM:GO module has its own oscillator; kept like the vendor image).
  ODDRX1 u_mipi_clk (
    .D0   ( 1'b1            ),
    .D1   ( 1'b0            ),
    .SCLK ( mipi_clk        ),
    .RST  ( !usr_clk_locked ),
    .Q    ( CAM_MCLK        )
  );

  logic init_done;

//------------------------------------------------------------------------------
// APB user register windows (Hololink external APB port n <-> address 0x(n+1)000_0000)
//------------------------------------------------------------------------------

  logic [`REG_INST-1:0] apb_psel;
  logic                 apb_penable;
  logic [31         :0] apb_paddr;
  logic [31         :0] apb_pwdata;
  logic                 apb_pwrite;
  logic [`REG_INST-1:0] apb_pready;
  logic [31         :0] apb_prdata [`REG_INST-1:0];
  logic [`REG_INST-1:0] apb_pserr;

  genvar i;

  // Windows 3, 4, 5, 7 are unused: answer at once with an invalid-read marker.
  generate
    for (i = 3; i < `REG_INST; i++) begin: apb_unused
      if (i != 6) begin
        assign apb_pready[i] = 1'b1;
        assign apb_prdata[i] = 32'hBADADD12;
        assign apb_pserr [i] = 1'b0;
      end
    end
  endgenerate

//------------------------------------------------------------------------------
// Lattice 10GbE host interface (window 0 = PCS, window 1 = MAC)
//------------------------------------------------------------------------------

  logic [`HOST_IF_INST-1  :0] hif_tx_axis_tvalid;
  logic [`HOST_IF_INST-1  :0] hif_tx_axis_tlast;
  logic [`HOST_WIDTH  -1  :0] hif_tx_axis_tdata [`HOST_IF_INST-1:0];
  logic [`HOSTKEEP_WIDTH-1:0] hif_tx_axis_tkeep [`HOST_IF_INST-1:0];
  logic [`HOSTUSER_WIDTH-1:0] hif_tx_axis_tuser [`HOST_IF_INST-1:0];
  logic [`HOST_IF_INST-1  :0] hif_tx_axis_tready;

  logic [`HOST_IF_INST-1  :0] hif_rx_axis_tvalid;
  logic [`HOST_IF_INST-1  :0] hif_rx_axis_tlast;
  logic [`HOST_WIDTH  -1  :0] hif_rx_axis_tdata [`HOST_IF_INST-1:0];
  logic [`HOSTKEEP_WIDTH-1:0] hif_rx_axis_tkeep [`HOST_IF_INST-1:0];
  logic [`HOSTUSER_WIDTH-1:0] hif_rx_axis_tuser [`HOST_IF_INST-1:0];
  logic [`HOST_IF_INST-1  :0] hif_rx_axis_tready;

  generate
    for (i=0; i<`HOST_IF_INST; i++) begin: ethernet_10gb
      eth_10gb_top #(
        .ID               ( 0                         )
      ) u_10gbe (
        .i_refclk_p       ( ETH_REFCLK_P              ),
        .i_refclk_n       ( ETH_REFCLK_N              ),
        .i_pad_rx_p       ( ETH_RXD_P                 ),
        .i_pad_rx_n       ( ETH_RXD_N                 ),
        .o_pad_tx_p       ( ETH_TXD_P                 ),
        .o_pad_tx_n       ( ETH_TXD_N                 ),
        .i_pcs_clk        ( pcs_clk                   ),
        .i_pcs_rst_n      ( pcs_rst_n                 ),
        .i_sys_rst_n      ( ~sys_rst                  ),
        .o_usr_clk        ( usr_clk               [i] ),
        .o_usr_clk_rdy    ( usr_clk_rdy           [i] ),
        .i_aclk           ( apb_clk                   ),
        .i_arst_n         (~apb_rst                   ),
        .i_pcs_apb_psel   ( apb_psel          [0+i*2] ),
        .i_pcs_apb_penable( apb_penable               ),
        .i_pcs_apb_paddr  ( apb_paddr                 ),
        .i_pcs_apb_pwdata ( apb_pwdata                ),
        .i_pcs_apb_pwrite ( apb_pwrite                ),
        .o_pcs_apb_pready ( apb_pready        [0+i*2] ),
        .o_pcs_apb_prdata ( apb_prdata        [0+i*2] ),
        .o_pcs_apb_pserr  ( apb_pserr         [0+i*2] ),
        .i_mac_apb_psel   ( apb_psel          [1+i*2] ),
        .i_mac_apb_penable( apb_penable               ),
        .i_mac_apb_paddr  ( apb_paddr                 ),
        .i_mac_apb_pwdata ( apb_pwdata                ),
        .i_mac_apb_pwrite ( apb_pwrite                ),
        .o_mac_apb_pready ( apb_pready        [1+i*2] ),
        .o_mac_apb_prdata ( apb_prdata        [1+i*2] ),
        .o_mac_apb_pserr  ( apb_pserr         [1+i*2] ),
        .i_pclk           ( hif_clk                   ),
        .i_prst_n         (~hif_rst                   ),
        .i_axis_tx_tvalid ( hif_tx_axis_tvalid    [i] ),
        .i_axis_tx_tlast  ( hif_tx_axis_tlast     [i] ),
        .i_axis_tx_tkeep  ( hif_tx_axis_tkeep     [i] ),
        .i_axis_tx_tdata  ( hif_tx_axis_tdata     [i] ),
        .i_axis_tx_tuser  ( hif_tx_axis_tuser     [i] ),
        .o_axis_tx_tready ( hif_tx_axis_tready    [i] ),
        .o_axis_rx_tvalid ( hif_rx_axis_tvalid    [i] ),
        .o_axis_rx_tlast  ( hif_rx_axis_tlast     [i] ),
        .o_axis_rx_tkeep  ( hif_rx_axis_tkeep     [i] ),
        .o_axis_rx_tdata  ( hif_rx_axis_tdata     [i] ),
        .o_axis_rx_tuser  ( hif_rx_axis_tuser     [i] ),
        .i_axis_rx_tready ( hif_rx_axis_tready    [i] ),
        .o_mac_interrupt  (                           ),
        .o_mac_tx_staten  (                           ),
        .o_mac_tx_statvec (                           ),
        .o_mac_rx_statvec (                           ),
        .o_mac_rx_staten  (                           ),
        .o_mac_crc_err    (                           ),
        .o_pcs_rxval      (                           ),
        .o_pcs_txrdy      (                           )
      );
    end
  endgenerate

//------------------------------------------------------------------------------
// QSPI configuration flash (SPI controller 0; controller 1 unused)
//------------------------------------------------------------------------------

  logic [3:0] flsh_spi_sdio_sync;

  data_sync    #(
    .DATA_WIDTH ( 4                  )
  ) spi_glitch_filter (
    .clk        ( hif_clk            ),
    .rst_n      (~hif_rst            ),
    .sync_in    (FLASH_SPI_SDIO      ),
    .sync_out   (flsh_spi_sdio_sync  )
  );

  logic [`SPI_INST-1:0] spi_csn;
  logic [`SPI_INST-1:0] spi_sck;
  logic [3          :0] spi_sdio_i [`SPI_INST-1:0];
  logic [3          :0] spi_sdio_o [`SPI_INST-1:0];
  logic [`SPI_INST-1:0] spi_oen;

  assign FLASH_SPI_MSCK = spi_sck  [0];
  assign FLASH_SPI_MCSN = spi_csn  [0];
  assign FLASH_SPI_SDIO = spi_oen  [0] ? spi_sdio_o[0] : 4'hz;
  assign spi_sdio_i[0]  = flsh_spi_sdio_sync;
  assign spi_sdio_i[1]  = 4'h0;

//------------------------------------------------------------------------------
// I2C: bus 0 = control/EEPROM, bus 1 = J1D camera
//------------------------------------------------------------------------------

  logic ctrl_i2c_scl_sync, ctrl_i2c_sda_sync;
  logic cam_i2c_scl_sync,  cam_i2c_sda_sync;

  glitch_filter  #(
    .DATA_WIDTH   ( 4                                     ),
    .RESET_VALUE  ( 1'b1                                  ),
    .FILTER_DEPTH ( 8                                     )
  ) i2c_glitch_filter (
    .clk          ( hif_clk                               ),
    .rst_n        (~hif_rst                               ),
    .sync_in      ({CTRL_I2C_SDA, CTRL_I2C_SCL, CAM_I2C_SDA, CAM_I2C_SCL}),
    .sync_out     ({ctrl_i2c_sda_sync, ctrl_i2c_scl_sync, cam_i2c_sda_sync, cam_i2c_scl_sync})
  );

  logic [`I2C_INST-1:0] i2c_scl;
  logic [`I2C_INST-1:0] i2c_sda;
  logic [`I2C_INST-1:0] i2c_scl_en;
  logic [`I2C_INST-1:0] i2c_sda_en;

  assign i2c_scl[0]   = i2c_scl_en[0] ? ctrl_i2c_scl_sync : 1'b0;
  assign i2c_sda[0]   = i2c_sda_en[0] ? ctrl_i2c_sda_sync : 1'b0;
  assign CTRL_I2C_SCL = i2c_scl_en[0] ? 1'bz : 1'b0;
  assign CTRL_I2C_SDA = i2c_sda_en[0] ? 1'bz : 1'b0;

  assign i2c_scl[1]   = i2c_scl_en[1] ? cam_i2c_scl_sync : 1'b0;
  assign i2c_sda[1]   = i2c_sda_en[1] ? cam_i2c_sda_sync : 1'b0;
  assign CAM_I2C_SCL  = i2c_scl_en[1] ? 1'bz : 1'b0;
  assign CAM_I2C_SDA  = i2c_sda_en[1] ? 1'bz : 1'b0;

//------------------------------------------------------------------------------
// Camera receiver (user window 2: 0x3000_0000; the D-PHY IP's LMMI registers are at +0x000..0x3FF)
//------------------------------------------------------------------------------

  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_clk;
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_axis_tvalid;
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_axis_tlast;
  logic [`DATAPATH_WIDTH-1:0] sif_rx_axis_tdata [`SENSOR_RX_IF_INST-1:0];
  logic [`DATAKEEP_WIDTH-1:0] sif_rx_axis_tkeep [`SENSOR_RX_IF_INST-1:0];
  logic [`DATAUSER_WIDTH-1:0] sif_rx_axis_tuser [`SENSOR_RX_IF_INST-1:0];
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_axis_tready;
  logic [15:0] sif_event;

  logic [31:0] dt_filter;      // MIPI_DT_CTRL (byte 0 used)
  logic [31:0] dt_seen;        // MIPI_DT_STAT (byte 0 used)
  logic        dt_seen_clear;

  assign sif_event      = {15'h0, sif_rx_axis_tlast[0]};
  assign dt_seen[31:8]  = 24'h0;

  generate
    for (i=0; i<`SENSOR_RX_IF_INST; i++) begin: cam_sensor_rcvr

      assign sif_rx_clk[i] = hif_clk;

      mipi_cam_rcvr_da322 u_cam_rcvr (
        .i_mipi_sync_clk    ( pcs_clk                ),
        .i_rst_n            ( RESET_N                ),
        .i_sclk             ( sif_rx_clk[i]          ),
        .i_srst             ( sif_rx_rst[i]          ),
        .i_pll_locked       ( usr_clk_locked         ),
        .i_apb_clk          ( apb_clk                ),
        .i_apb_rst          ( apb_rst                ),
        .i_apb_sel          ( apb_psel           [2] ),
        .i_apb_enable       ( apb_penable            ),
        .i_apb_addr         ( apb_paddr              ),
        .i_apb_wdata        ( apb_pwdata             ),
        .i_apb_write        ( apb_pwrite             ),
        .o_apb_ready        ( apb_pready         [2] ),
        .o_apb_rdata        ( apb_prdata         [2] ),
        .o_apb_serr         ( apb_pserr          [2] ),
        .o_axis_tvalid      ( sif_rx_axis_tvalid [i] ),
        .o_axis_tlast       ( sif_rx_axis_tlast  [i] ),
        .o_axis_tdata       ( sif_rx_axis_tdata  [i] ),
        .o_axis_tkeep       ( sif_rx_axis_tkeep  [i] ),
        .o_axis_tuser       ( sif_rx_axis_tuser  [i] ),
        .o_axis_tidx        (                        ),
        .i_axis_tready      ( sif_rx_axis_tready [i] ),
        .i_dt_filter        ( dt_filter        [7:0] ),
        .i_dt_seen_clear    ( dt_seen_clear          ),
        .o_dt_seen          ( dt_seen          [7:0] ),
        .mipi_cam_clk_n_io  ( MIPI_CAM_CLK_N         ),
        .mipi_cam_clk_p_io  ( MIPI_CAM_CLK_P         ),
        .mipi_cam_data_n_io ( MIPI_CAM_DATA_N        ),
        .mipi_cam_data_p_io ( MIPI_CAM_DATA_P        )
     );

    end
  endgenerate

//------------------------------------------------------------------------------
// USER_CSR / MIPI_DT_CTRL / MIPI_DT_STAT / BUILD_ID (user window 6: 0x7000_0000)
//------------------------------------------------------------------------------

  da322_user_csr #(
    .BUILD_ID        ( 32'hDA32_2101  )   // 1-camera image, revision 1
  ) u_user_csr (
    .i_apb_clk       ( apb_clk        ),
    .i_apb_rst       ( apb_rst        ),
    .i_apb_sel       ( apb_psel   [6] ),
    .i_apb_enable    ( apb_penable    ),
    .i_apb_addr      ( apb_paddr      ),
    .i_apb_wdata     ( apb_pwdata     ),
    .i_apb_write     ( apb_pwrite     ),
    .o_apb_ready     ( apb_pready [6] ),
    .o_apb_rdata     ( apb_prdata [6] ),
    .o_apb_serr      ( apb_pserr  [6] ),
    .o_dt_filter     ( dt_filter      ),
    .o_dt_seen_clear ( dt_seen_clear  ),
    .i_dt_seen       ( dt_seen        )
  );

//------------------------------------------------------------------------------
// Hololink IP
//------------------------------------------------------------------------------

  HOLOLINK_top #(
    .BUILD_REV         ( BUILD_REV          )
  ) u_hololink_top (
    .i_sys_rst         ( sys_rst            ),
    .i_apb_clk         ( apb_clk            ),
    .o_apb_rst         ( apb_rst            ),
    .o_apb_psel        ( apb_psel           ),
    .o_apb_penable     ( apb_penable        ),
    .o_apb_paddr       ( apb_paddr          ),
    .o_apb_pwdata      ( apb_pwdata         ),
    .o_apb_pwrite      ( apb_pwrite         ),
    .i_apb_pready      ( apb_pready         ),
    .i_apb_prdata      ( apb_prdata         ),
    .i_apb_pserr       ( apb_pserr          ),
`ifndef ENUM_EEPROM
    .i_mac_addr        ( '{`MAC_ADDR}       ),
    .i_board_sn        ( '{`BOARD_SN}       ),
    .i_enum_vld        ( 1'b1               ),
`endif
    .o_init_done       ( init_done          ),
    .i_sif_rx_clk      ( sif_rx_clk         ),
    .o_sif_rx_rst      ( sif_rx_rst         ),
    .i_sif_axis_tvalid ( sif_rx_axis_tvalid ),
    .i_sif_axis_tlast  ( sif_rx_axis_tlast  ),
    .i_sif_axis_tdata  ( sif_rx_axis_tdata  ),
    .i_sif_axis_tkeep  ( sif_rx_axis_tkeep  ),
    .i_sif_axis_tuser  ( sif_rx_axis_tuser  ),
    .o_sif_axis_tready ( sif_rx_axis_tready ),
    .i_sif_event       ( sif_event          ),
    .i_hif_clk         ( hif_clk            ),
    .o_hif_rst         ( hif_rst            ),
    .i_hif_axis_tvalid ( hif_rx_axis_tvalid ),
    .i_hif_axis_tlast  ( hif_rx_axis_tlast  ),
    .i_hif_axis_tdata  ( hif_rx_axis_tdata  ),
    .i_hif_axis_tkeep  ( hif_rx_axis_tkeep  ),
    .i_hif_axis_tuser  ( hif_rx_axis_tuser  ),
    .o_hif_axis_tready ( hif_rx_axis_tready ),
    .o_hif_axis_tvalid ( hif_tx_axis_tvalid ),
    .o_hif_axis_tlast  ( hif_tx_axis_tlast  ),
    .o_hif_axis_tdata  ( hif_tx_axis_tdata  ),
    .o_hif_axis_tkeep  ( hif_tx_axis_tkeep  ),
    .o_hif_axis_tuser  ( hif_tx_axis_tuser  ),
    .i_hif_axis_tready ( hif_tx_axis_tready ),
    .o_spi_csn         ( spi_csn            ),
    .o_spi_sck         ( spi_sck            ),
    .i_spi_sdio        ( spi_sdio_i         ),
    .o_spi_sdio        ( spi_sdio_o         ),
    .o_spi_oen         ( spi_oen            ),
    .i_i2c_scl         ( i2c_scl            ),
    .i_i2c_sda         ( i2c_sda            ),
    .o_i2c_scl_en      ( i2c_scl_en         ),
    .o_i2c_sda_en      ( i2c_sda_en         ),
    .o_gpio            ( gpio_out           ),
    .o_gpio_dir        ( gpio_dir           ),
    .i_gpio            ( gpio_in            ),
    .o_sw_sys_rst      ( sw_sys_rst         ),
    .o_sw_sen_rst      ( sw_sen_rst         ),
    .i_ptp_clk         ( ptp_clk            ),
    .o_ptp_rst         ( ptp_rst            ),
    .o_ptp_sec         ( ptp_sec            ),
    .o_ptp_nanosec     ( ptp_nsec           ),
    .o_pps             ( sys_pps            )
  );

endmodule
