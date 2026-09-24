// Tauro DA322 top level: four MIPI CSI-2 cameras -> Hololink IP -> 10G SFP+.
//
// Derived from holoscan-sensor-bridge 2.7.0 fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/rtl/top/FPGA_top.sv
// (Apache-2.0, NVIDIA), which targets the two-camera Tauro DA326 on the same FPGA/pin family. Changes:
//   * four camera receivers (J1A..J1D) with Tauro's CSI data-type filter, behind user window 2
//     (0x3000_Y000, Y = camera; the D-PHY IP's lane register is 0x3000_Y028 as in the vendor image)
//   * five I2C buses: 0 = control/EEPROM, 1..4 = camera J1A..J1D (host convention: I2C bus 1 + camera)
//   * CAM_EN[k] driven by Hololink GPIO k (host convention), MFP header J4/J5 on GPIO 4..15
//   * USER_CSR / MIPI_DT_CTRL / MIPI_DT_STAT at 0x7000_0000 (da322_user_csr)
//   * no MAX96716 deserializer, no PoC controller, no VSYNC generator (DA326-only features)
`include "HOLOLINK_def.svh"

module FPGA_top
  import HOLOLINK_pkg::*;
  import apb_pkg::*;
#(
  parameter BUILD_REV = 48'h0
)(
  input           RESET_N,                 // H1 (pulled up; no button on the DA322 is fine)
  // 10GbE SFP+
  input           ETH_REFCLK_P,            // D10 / E10, 161.1328125 MHz
  input           ETH_REFCLK_N,
  input           ETH_RXD_P,
  input           ETH_RXD_N,
  output          ETH_TXD_P,
  output          ETH_TXD_N,
  output          SFP_TX_DIS,

  // MIPI CSI-2, index 0..3 = J1A, J1B, J1C, J1D
  inout   [3:0]   MIPI_CAM_CLK_P,
  inout   [3:0]   MIPI_CAM_CLK_N,
  inout   [3:0]   MIPI_CAM_DATA_P [3:0],
  inout   [3:0]   MIPI_CAM_DATA_N [3:0],

  // I2C
  inout           CTRL_I2C_SCL,
  inout           CTRL_I2C_SDA,
  inout   [3:0]   CAM_I2C_SCL,
  inout   [3:0]   CAM_I2C_SDA,

  // Camera enable (pin 17 of each 22-pin connector) and reference clock (pin 18)
  output  [3:0]   CAM_EN,
  output  [3:0]   CAM_MCLK,

  // QSPI configuration flash
  output          FLASH_SPI_MCSN,
  output          FLASH_SPI_MSCK,
  inout   [3:0]   FLASH_SPI_SDIO,

  // MFP headers: GPIO[10:0] = J4 pins 1..11 (GPIO0..GPIO10), GPIO[11] = J5 pin 1 (GPIO11)
  inout   [11:0]  GPIO
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
  logic        sys_pps;

  assign usr_clk_locked = &usr_clk_rdy;

  clk_n_rst u_clk_n_rst (
    .i_refclk      ( usr_clk [0]    ), // pcs user clock output
    .i_locked      ( usr_clk_locked ), // pcs user clock locked

    .o_mipi_clk    ( mipi_clk       ), // 27.043MHz clock for MIPI
    .o_pcs_clk     ( pcs_clk        ), // pcs calibration clock
    .o_hif_clk     ( hif_clk        ), // host interface clock
    .o_apb_clk     ( apb_clk        ), // apb interface clock
    .o_ptp_clk     ( ptp_clk        ), // ptp interface clock

    .i_ptp_nsec    ( ptp_nsec       ),
    .o_ptp_cam_clk ( ptp_cam_clk    ), // ptp 24MHz clock
    .i_pb_rst_n    ( RESET_N        ), // asynchronous active low board reset
    .i_sw_rst      ( sw_sys_rst     ), // software controlled active high reset

    .o_sys_rst     ( sys_rst        ), // system active high reset
    .o_pcs_rst_n   ( pcs_rst_n      )  // ethernet pcs active low reset
  );

//------------------------------------------------------------------------------
// Board control
//------------------------------------------------------------------------------

  assign SFP_TX_DIS = 1'b0;

  // Camera enable: Hololink GPIO k (host writes GPIO k HIGH to power camera k, as with the vendor image).
  assign CAM_EN = gpio_out[3:0];

  // Camera reference clock on pin 18 of every connector (the FSM:GO modules carry their own INCK
  // oscillator; the clock is provided for modules that need it, as the vendor image does).
  genvar m;
  generate
    for (m = 0; m < 4; m++) begin: cam_mclk
      ODDRX1 u_mipi_clk (
        .D0   ( 1'b1            ),
        .D1   ( 1'b0            ),
        .SCLK ( mipi_clk        ),
        .RST  ( !usr_clk_locked ),
        .Q    ( CAM_MCLK[m]     )
      );
    end
  endgenerate

  logic init_done;

//------------------------------------------------------------------------------
// APB user register windows (Hololink external APB ports, index n <-> address 0x(n+1)000_0000)
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

  // Windows 3, 4, 5, 7 (0x4000_0000, 0x5000_0000, 0x6000_0000, 0x8000_0000) are unused: answer at once.
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
// QSPI configuration flash
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
  assign spi_sdio_i[1]  = 4'h0;  // second SPI controller unused on the DA322

//------------------------------------------------------------------------------
// I2C: bus 0 = control/EEPROM (H7/H6), buses 1..4 = cameras J1A..J1D
//------------------------------------------------------------------------------

  logic       ctrl_i2c_scl_sync, ctrl_i2c_sda_sync;
  logic [3:0] cam_i2c_scl_sync,  cam_i2c_sda_sync;

  glitch_filter  #(
    .DATA_WIDTH   ( 10                                    ),
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

  generate
    for (i = 0; i < 4; i++) begin: cam_i2c
      assign i2c_scl[1+i]   = i2c_scl_en[1+i] ? cam_i2c_scl_sync[i] : 1'b0;
      assign i2c_sda[1+i]   = i2c_sda_en[1+i] ? cam_i2c_sda_sync[i] : 1'b0;
      assign CAM_I2C_SCL[i] = i2c_scl_en[1+i] ? 1'bz : 1'b0;
      assign CAM_I2C_SDA[i] = i2c_sda_en[1+i] ? 1'bz : 1'b0;
    end
  endgenerate

//------------------------------------------------------------------------------
// Camera receivers (user window 2: 0x3000_Y000, Y = camera 0..3, sub-decoded on address bits 13:12)
//------------------------------------------------------------------------------

  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_clk;
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_axis_tvalid;
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_axis_tlast;
  logic [`DATAPATH_WIDTH-1:0] sif_rx_axis_tdata [`SENSOR_RX_IF_INST-1:0];
  logic [`DATAKEEP_WIDTH-1:0] sif_rx_axis_tkeep [`SENSOR_RX_IF_INST-1:0];
  logic [`DATAUSER_WIDTH-1:0] sif_rx_axis_tuser [`SENSOR_RX_IF_INST-1:0];
  logic [`SENSOR_RX_IF_INST-1:0] sif_rx_axis_tready;
  logic [15:0] sif_event;

  logic [31:0] dt_filter;                        // MIPI_DT_CTRL, one byte per camera
  logic [31:0] dt_seen;                          // MIPI_DT_STAT
  logic        dt_seen_clear;

  logic [3:0]  rcvr_apb_sel;
  logic [3:0]  rcvr_apb_ready;
  logic [31:0] rcvr_apb_rdata [3:0];
  logic [3:0]  rcvr_apb_serr;

  assign sif_event = {12'h0, sif_rx_axis_tlast[3:0]};

  // Window 2 fan-out and response mux by camera index.
  always_comb begin
    for (int k = 0; k < 4; k++) rcvr_apb_sel[k] = apb_psel[2] && (apb_paddr[13:12] == k[1:0]);
    apb_pready[2] = rcvr_apb_ready[apb_paddr[13:12]];
    apb_prdata[2] = rcvr_apb_rdata[apb_paddr[13:12]];
    apb_pserr [2] = rcvr_apb_serr [apb_paddr[13:12]];
  end

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
        .i_apb_sel          ( rcvr_apb_sel       [i] ),
        .i_apb_enable       ( apb_penable            ),
        .i_apb_addr         ( apb_paddr              ),
        .i_apb_wdata        ( apb_pwdata             ),
        .i_apb_write        ( apb_pwrite             ),
        .o_apb_ready        ( rcvr_apb_ready     [i] ),
        .o_apb_rdata        ( rcvr_apb_rdata     [i] ),
        .o_apb_serr         ( rcvr_apb_serr      [i] ),
        .o_axis_tvalid      ( sif_rx_axis_tvalid [i] ),
        .o_axis_tlast       ( sif_rx_axis_tlast  [i] ),
        .o_axis_tdata       ( sif_rx_axis_tdata  [i] ),
        .o_axis_tkeep       ( sif_rx_axis_tkeep  [i] ),
        .o_axis_tuser       ( sif_rx_axis_tuser  [i] ),
        .o_axis_tidx        (                        ),
        .i_axis_tready      ( sif_rx_axis_tready [i] ),
        .i_dt_filter        ( dt_filter    [8*i +: 8] ),
        .i_dt_seen_clear    ( dt_seen_clear          ),
        .o_dt_seen          ( dt_seen      [8*i +: 8] ),
        .mipi_cam_clk_n_io  ( MIPI_CAM_CLK_N     [i] ),
        .mipi_cam_clk_p_io  ( MIPI_CAM_CLK_P     [i] ),
        .mipi_cam_data_n_io ( MIPI_CAM_DATA_N    [i] ),
        .mipi_cam_data_p_io ( MIPI_CAM_DATA_P    [i] )
     );

    end
  endgenerate

//------------------------------------------------------------------------------
// USER_CSR / MIPI_DT_CTRL / MIPI_DT_STAT (user window 6: 0x7000_0000)
//------------------------------------------------------------------------------

  da322_user_csr u_user_csr (
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
// GPIO: 0..3 camera enables (outputs above), 4..15 = MFP header pins
//------------------------------------------------------------------------------

  logic [15:0] gpio_in;
  logic [15:0] gpio_dir;   // 0 = output, 1 = input
  logic [11:0] gpio_pad_sync;

  data_sync #(
    .DATA_WIDTH  ( 12   ),
    .RESET_VALUE ( 1'b0 )
  ) gpio_synchronizer (
    .clk         ( hif_clk       ),
    .rst_n       (~hif_rst       ),
    .sync_in     ( GPIO          ),
    .sync_out    ( gpio_pad_sync )
  );

  assign gpio_in[3:0]  = gpio_out[3:0];   // camera enables read back what is driven
  assign gpio_in[15:4] = gpio_pad_sync;

  generate
    for (i = 0; i < 12; i++) begin: mfp_gpio
      assign GPIO[i] = gpio_dir[4+i] ? 1'bz : gpio_out[4+i];
    end
  endgenerate

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
