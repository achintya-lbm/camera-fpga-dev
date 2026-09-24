#!/usr/bin/env bash
# Assembles a Radiant work tree for the DA322 bitstream and (unless --no-build) runs the build.
#
#   fpga/radiant/assemble_da322.sh [--work DIR] [--radiant DIR] [--license FILE] [--no-build]
#
# Defaults: work tree ~/fpga_build/da322, Radiant ~/lscc/radiant/2026.1, licence
# $RADIANT/license/license.dat (or $LM_LICENSE_FILE). The Hololink IP and the reference design come
# from the Bazel repository @hsb_fpga (holoscan-sensor-bridge 2.7.0); our RTL and constraints are
# linked from fpga/rtl/da322 and fpga/boards/da322. A full build takes tens of minutes and needs a
# CertusPro-NX-capable licence (subscription or 60-day evaluation), see fpga/README.md.
set -euo pipefail

root=$(cd "$(dirname "$0")/../.." && pwd)
work="$HOME/fpga_build/da322"
radiant="$HOME/lscc/radiant/2026.1"
license=""
build=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --work) work="$2"; shift 2 ;;
    --radiant) radiant="$2"; shift 2 ;;
    --license) license="$2"; shift 2 ;;
    --no-build) build=0; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done

cd "$root"
bazel build @hsb_fpga//:fpga_sources >/dev/null
hsb=$(bazel info output_base)/external/+camera_fpga_dev_repositories+hsb_fpga
[[ -d "$hsb/fpga/nv_hsb_ip" ]] || { echo "Hololink IP sources not found under $hsb" >&2; exit 1; }

mkdir -p "$work/build/ip"
ln -sfn "$hsb" "$work/hsb"
ln -sfn "$root/fpga/rtl/da322" "$work/rtl"
ln -sfn "$root/fpga/boards/da322" "$work/constraints"
cp "$root/fpga/radiant/da322_build.tcl" "$work/build/build.tcl"
# IP configurations of the reference design (the generated IP lands next to them).
ref="$hsb/fpga/nv_mipi_ref_design/mipi_cpnx_ref_design/build/ip"
for d in "$ref"/*/; do
  name=$(basename "$d")
  mkdir -p "$work/build/ip/$name"
  cp -n "$d"/*.cfg "$work/build/ip/$name/" 2>/dev/null || true
done
echo "work tree: $work"
ls -la "$work"

if [[ -z "$license" ]]; then
  if [[ -f "$radiant/license/license.dat" ]]; then license="$radiant/license/license.dat"; fi
fi
if [[ $build -eq 0 ]]; then exit 0; fi
if [[ -z "$license" && -z "${LM_LICENSE_FILE:-}" ]]; then
  echo "no Radiant licence: put license.dat in $radiant/license/ or pass --license FILE" >&2
  exit 1
fi
export RADIANT_PATH="$radiant"
export LM_LICENSE_FILE="${license:+$license:}${LM_LICENSE_FILE:-}"
cd "$work/build"
log="$work/build/build_$(date +%Y%m%d_%H%M%S).log"
echo "building with $RADIANT_PATH/bin/lin64/radiantc, log $log"
"$RADIANT_PATH/bin/lin64/radiantc" build.tcl 2>&1 | tee "$log"
grep -E "BITFILE:|ERROR|Error" "$log" | tail -n 20
