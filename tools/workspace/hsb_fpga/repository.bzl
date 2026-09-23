"""holoscan-sensor-bridge 2.7.0 source tree, fetched for its FPGA sources (Apache-2.0):

  fpga/nv_hsb_ip/                 Hololink IP core, SystemVerilog (HOLOLINK_REV 0x2606, back-compat 0x2603)
  fpga/nv_hsb_ip_simple_tb/       IP testbench
  fpga/nv_mipi_ref_design/        CertusPro-NX MIPI reference design (Tauro DA326 pinout = DA322 J1D/J1B)
  fpga/lattice/                   Lattice CPNX100 Ethernet bridge board design + Radiant build scripts

The host library still comes from //tools/workspace/hololink (2.5.0-PB6 + Tauro patch); this repository
is only consumed by the FPGA flow (fpga/, DESIGN.md §12).
"""

load("//tools/workspace:archive.bzl", "archive_repository")

_TAG = "2.7.0"

_SHA256 = "ca0b86a3ac59db087217d33d4e93f161799d07e67cab5d8911718a9833db0037"

def hsb_fpga_repository(name):
    archive_repository(
        name = name,
        urls = ["https://github.com/nvidia-holoscan/holoscan-sensor-bridge/archive/refs/tags/{t}.tar.gz".format(t = _TAG)],
        sha256 = _SHA256,
        strip_prefix = "holoscan-sensor-bridge-" + _TAG,
        build_file = "//tools/workspace/hsb_fpga:package.BUILD.bazel",
    )
