# Tauro DA322 timing constraints — the DA326 reference fpga_cpnx.sdc extended to four camera receivers.
create_clock -name {ETH_REFCLK_P} -period 6.2060606 [get_ports ETH_REFCLK_P]

# Internal clock nets
create_clock -name {usr_clk} -period 3.1030303030303 [get_pins {ethernet_10gb[0].u_10gbe/o_usr_clk}]
create_clock -name {apb_clk} -period 51.2 [get_pins u_clk_n_rst/o_apb_clk]
create_clock -name {hif_clk} -period 6.4 [get_pins u_hololink_top/i_hif_clk]
create_clock -name {ptp_clk} -period 9.95554401 [get_pins u_clk_n_rst/o_ptp_clk]
create_clock -name {u_pcs_clk_hf_clk_out_o} -period 6.667 -waveform {0.000 3.333} [get_pins u_clk_n_rst/u_pcs_clk/lscc_osc_inst/gen_osca.u_OSC_A.OSCA_inst/HFCLKOUT]
create_clock -name {sif_clk} -period 6.4 [get_pins u_hololink_top/i_sif_clk]

# Per-camera D-PHY byte clocks (1.5 Gbps line rate -> 187.5 MHz byte clock), edge clocks and lane clocks
create_clock -name {clk_byte_hs_0} -period 5.33333333333333 [get_nets {cam_sensor_rcvr[0].u_cam_rcvr/clk_byte_hs_o}]
create_clock -name {clk_byte_hs_1} -period 5.33333333333333 [get_nets {cam_sensor_rcvr[1].u_cam_rcvr/clk_byte_hs_o}]
create_clock -name {clk_byte_hs_2} -period 5.33333333333333 [get_nets {cam_sensor_rcvr[2].u_cam_rcvr/clk_byte_hs_o}]
create_clock -name {clk_byte_hs_3} -period 5.33333333333333 [get_nets {cam_sensor_rcvr[3].u_cam_rcvr/clk_byte_hs_o}]
create_clock -name {eclkout_0} -period 3.47222222222222 [get_pins {cam_sensor_rcvr[0].u_cam_rcvr/mipi_rx_ip/lscc_dphy_rx_inst/NOCIL_TOP.u_dphy_rx_core/u_dphy_rx_wrap/SOFT_RX.SOFT_RX_NX.u_lscc_mipi_dphy_soft_rx/u_clksync/ECLKOUT}]
create_clock -name {eclkout_1} -period 3.47222222222222 [get_pins {cam_sensor_rcvr[1].u_cam_rcvr/mipi_rx_ip/lscc_dphy_rx_inst/NOCIL_TOP.u_dphy_rx_core/u_dphy_rx_wrap/SOFT_RX.SOFT_RX_NX.u_lscc_mipi_dphy_soft_rx/u_clksync/ECLKOUT}]
create_clock -name {eclkout_2} -period 3.47222222222222 [get_pins {cam_sensor_rcvr[2].u_cam_rcvr/mipi_rx_ip/lscc_dphy_rx_inst/NOCIL_TOP.u_dphy_rx_core/u_dphy_rx_wrap/SOFT_RX.SOFT_RX_NX.u_lscc_mipi_dphy_soft_rx/u_clksync/ECLKOUT}]
create_clock -name {eclkout_3} -period 3.47222222222222 [get_pins {cam_sensor_rcvr[3].u_cam_rcvr/mipi_rx_ip/lscc_dphy_rx_inst/NOCIL_TOP.u_dphy_rx_core/u_dphy_rx_wrap/SOFT_RX.SOFT_RX_NX.u_lscc_mipi_dphy_soft_rx/u_clksync/ECLKOUT}]
create_clock -name {MIPI_CAM_CLK_P_0} -period 1.33333333333333 [get_ports {MIPI_CAM_CLK_P[0]}]
create_clock -name {MIPI_CAM_CLK_P_1} -period 1.33333333333333 [get_ports {MIPI_CAM_CLK_P[1]}]
create_clock -name {MIPI_CAM_CLK_P_2} -period 1.33333333333333 [get_ports {MIPI_CAM_CLK_P[2]}]
create_clock -name {MIPI_CAM_CLK_P_3} -period 1.33333333333333 [get_ports {MIPI_CAM_CLK_P[3]}]

set_clock_groups -group [get_clocks {apb_clk hif_clk sif_clk}] -group [get_clocks {ETH_REFCLK_P usr_clk}] -group [get_clocks u_pcs_clk_hf_clk_out_o] \
  -group [get_clocks {clk_byte_hs_0}] -group [get_clocks {clk_byte_hs_1}] -group [get_clocks {clk_byte_hs_2}] -group [get_clocks {clk_byte_hs_3}] \
  -group [get_clocks {eclkout_0}] -group [get_clocks {eclkout_1}] -group [get_clocks {eclkout_2}] -group [get_clocks {eclkout_3}] \
  -group [get_clocks {MIPI_CAM_CLK_P_0}] -group [get_clocks {MIPI_CAM_CLK_P_1}] -group [get_clocks {MIPI_CAM_CLK_P_2}] -group [get_clocks {MIPI_CAM_CLK_P_3}] \
  -group [get_clocks {ptp_clk}] -asynchronous
