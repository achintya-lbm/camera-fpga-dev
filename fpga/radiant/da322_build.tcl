#----------------------------------------------------------------------------------------------
# Tauro DA322 — Radiant build script (run by fpga/radiant/assemble_da322.sh from <work>/build).
# Derived from holoscan-sensor-bridge 2.7.0 fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/build/build.tcl.
#
# Directory layout expected around this script (created by assemble_da322.sh):
#   <work>/hsb/fpga/nv_hsb_ip/                       Hololink IP (SystemVerilog)
#   <work>/hsb/fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/rtl/{clk_n_rst,eth_10gb}/   reused as is
#   <work>/rtl/                                      fpga/rtl/da322 (FPGA_top, receiver, CSR, HOLOLINK_def.svh)
#   <work>/constraints/                              fpga/boards/da322 (da322.pdc, da322.sdc)
#   <work>/build/ip/<name>/<name>.cfg                IP configurations copied from the reference design
#   <work>/build/<date>/fpga_da322/                  Radiant project (created here)
#----------------------------------------------------------------------------------------------

set prj_name    fpga_da322
set vendor_ID   0322
set date        [clock format [clock seconds] -format "%m%d%Y"]

set prj_dir $date
file mkdir $prj_dir/$prj_name
file mkdir $prj_dir/bitfile
cd $prj_dir/$prj_name

prj_create -name $prj_name \
           -impl "impl_1"  \
           -dev LFCPNX-100-9CBG256I \
           -performance "9_High-Performance_1.0V" \
           -synthesis "synplify"

prj_set_strategy_value -strategy Strategy1 syn_pipelining_retiming=Pipelining and Retiming \
                                            syn_arrange_vhdl_files=False \
                                            syn_critical_path_num=100 \
                                            syn_start_end_pt_num=20 \
                                            syn_frequency=157 \
                                            par_core_number=8 \
                                            par_place_iterator=8 \
                                            par_stop_zero=True

prj_set_strategy_value -strategy Strategy1 syn_ram_rw_check=False syn_allow_dup_modules=True syn_default_enum_encode=Onehot syn_res_sharing=False

prj_set_strategy_value -strategy Strategy1 par_spd_hld_opt=9_High-Performance_1.0V par_spd_setup_opt=9_High-Performance_1.0V

# Uncomment to get a bitstream even when timing fails (first bring-up only):
# prj_set_strategy_value -strategy Strategy1 tmchk_enable_check=False

proc addFiles { basedir pattern } {
    set basedir [string trimright [file join [file normalize $basedir] { }]]
    set patternToExclude "pkg"
    foreach file [glob -nocomplain -type {f r} -path $basedir $pattern]  {
        if {[string first $patternToExclude [file tail $file]] >= 0} {
            puts $file
            prj_add_source $file
        }
    }
    foreach file [glob -nocomplain -type {f r} -path $basedir $pattern]  {
        if {[string first $patternToExclude [file tail $file]] == -1} {
            puts $file
            prj_add_source $file
        }
    }
}

#----------------------------------------------------------------------------------------------
# Paths (relative to <work>/build/<date>/fpga_da322)
#----------------------------------------------------------------------------------------------

set work_dir "../../../"
set hsb_dir  "$work_dir/hsb/fpga/"
set ref_dir  "$hsb_dir/nv_mipi_ref_design/mipi_cpnx_ref_design/"
set ip_cfg   "../../ip"

#----------------------------------------------------------------------------------------------
# IP generation (same cores and settings as the reference design; one D-PHY RX module shared by
# the four receiver instances)
#----------------------------------------------------------------------------------------------

ip_catalog_install -vlnv latticesemi.com:ip:ten_gbe_mac:1.1.0
ip_catalog_install -vlnv latticesemi.com:ip:ten_gbe_pcs:1.3.0
ip_catalog_install -vlnv latticesemi.com:ip:dphy_rx:2.1.0

set dev_args [list -a LFCPNX -p LFCPNX-100 -t CBG256 -sp 9_High-Performance_1.0V -op COM]

exec ipgen -o $ip_cfg/mipi_rx_ip     -vlnv latticesemi.com:ip:dphy_rx:2.1.0     -name mipi_rx_ip     -cfg "$ip_cfg/mipi_rx_ip/mipi_rx_ip.cfg"         {*}$dev_args
exec ipgen -o $ip_cfg/mipiClkOutPLL  -ip $env(RADIANT_PATH)/ip/lifcl/pll        -name mipiClkOutPLL  -cfg "$ip_cfg/mipiClkOutPLL/mipiClkOutPLL.cfg"   {*}$dev_args
exec ipgen -o $ip_cfg/eclk_mipi_pll  -ip $env(RADIANT_PATH)/ip/lifcl/pll        -name eclk_mipi_pll  -cfg "$ip_cfg/eclk_mipi_pll/eclk_mipi_pll.cfg"   {*}$dev_args
exec ipgen -o $ip_cfg/osc_clk        -ip $env(RADIANT_PATH)/ip/lifcl/osc        -name osc_clk        -cfg "$ip_cfg/osc_clk/osc_clk.cfg"               {*}$dev_args
exec ipgen -o $ip_cfg/ptp_sensor_pll -ip $env(RADIANT_PATH)/ip/lifcl/pll        -name ptp_sensor_pll -cfg "$ip_cfg/ptp_sensor_pll/ptp_sensor_pll.cfg" {*}$dev_args
exec ipgen -o $ip_cfg/eth_10gb_mac   -vlnv latticesemi.com:ip:ten_gbe_mac:1.1.0 -name eth_10gb_mac   -cfg "$ip_cfg/eth_10gb_mac/eth_10gb_mac.cfg"     {*}$dev_args
exec ipgen -o $ip_cfg/eth_10gb_pcs_0 -vlnv latticesemi.com:ip:ten_gbe_pcs:1.3.0 -name eth_10gb_pcs_0 -cfg "$ip_cfg/eth_10gb_pcs_0/eth_10gb_pcs_0.cfg" {*}$dev_args
exec ipgen -o $ip_cfg/eth_10gb_pcs_1 -vlnv latticesemi.com:ip:ten_gbe_pcs:1.3.0 -name eth_10gb_pcs_1 -cfg "$ip_cfg/eth_10gb_pcs_1/eth_10gb_pcs_1.cfg" {*}$dev_args
exec ipgen -o $ip_cfg/lvds_ddr_rx    -ip $env(RADIANT_PATH)/ip/lifcl/ddr        -name lvds_ddr_rx    -cfg "$ip_cfg/lvds_ddr_rx/lvds_ddr_rx.cfg"       {*}$dev_args

#----------------------------------------------------------------------------------------------
# Sources
#----------------------------------------------------------------------------------------------

# Generated IP
addFiles $ip_cfg "*/*ipx"

# DA322 configuration header (shadows the reference HOLOLINK_def.svh via the include path below)
addFiles "$work_dir/rtl" "*.svh"

# Hololink IP and its reference blocks (data_sync, glitch_filter, vsync, ...)
addFiles $hsb_dir "nv_hsb_ip/*/*v"
addFiles $hsb_dir "nv_hsb_ip/ref_design/*/*v"

# Reference design blocks reused unchanged: clocking/reset and the 10G Ethernet wrapper
addFiles $ref_dir "rtl/clk_n_rst/*v"
addFiles $ref_dir "rtl/eth_10gb/*v"

# DA322 RTL: top, camera receiver with data-type filter, user CSR
addFiles "$work_dir/rtl" "*.sv"

# Constraints
addFiles "$work_dir/constraints" "da322.pdc"
addFiles "$work_dir/constraints" "da322.sdc"

#----------------------------------------------------------------------------------------------
# Tool options
#----------------------------------------------------------------------------------------------

prj_set_impl_opt -impl "impl_1" "top" "FPGA_top"
prj_set_impl_opt -impl "impl_1" "VerilogStandard" "System Verilog"
prj_set_impl_opt -impl "impl_1" "include path" $work_dir/rtl
prj_set_impl_opt -impl "impl_1" -append "include path" $hsb_dir/nv_hsb_ip/top

set dateTime [clock format [clock seconds] -format "%Y%m%d%H%M%S"]
set dateTime [format %x $dateTime]
prj_set_impl_opt -impl "impl_1" "HDL_PARAM" "BUILD_REV=48'h$dateTime$vendor_ID"

#----------------------------------------------------------------------------------------------
# Run
#----------------------------------------------------------------------------------------------

prj_run Synthesis -impl impl_1 -task SynTrace
prj_run Map -impl impl_1 -task MapTrace
prj_run PAR -impl impl_1 -task PARTrace
prj_run Export -impl impl_1 -task Bitgen
prj_save
prj_close

file copy ./impl_1/${prj_name}_impl_1.bit ../bitfile/fpga_da322_$dateTime.bit
puts "BITFILE: [file normalize ../bitfile/fpga_da322_$dateTime.bit]"
