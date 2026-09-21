#!/usr/bin/env bash
# Capture sample frames from one DA322 camera port in every IMX676 mode at the highest frame rate the
# mode catalogue allows, then decode them and write a markdown catalogue.
#
#   tools/capture/capture_modes.sh [--port J1D] [--receiver roce|linux] [--ip 192.168.0.2]
#                                  [--out captures/imx676_cam4] [--duration 12] [--modes "A B C"]
#                                  [--fps N] [--exposure-ms E] [--gain-db G]
#
# Per mode: bandwidth_test with --dump-dir (3 raw frames + sidecars, CSV, JSON summary), then
# tools/py:raw_frame for previews/statistics. Requires the apps built (bazel build //apps/... //tools/py:raw_frame).
set -euo pipefail
port=J1D; receiver=linux; ip=192.168.0.2; out=captures/imx676_cam4; duration=12
modes="FULL_RAW10 FULL_RAW12 BIN2_RAW12 CROP_3552X2160_RAW10 CROP_1280X720_RAW10"
fps=1000   # above every ceiling: PlanTiming clamps VMAX to its minimum, i.e. the mode's maximum rate
exposure_ms=3; gain_db=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) port="$2"; shift 2 ;;
    --receiver) receiver="$2"; shift 2 ;;
    --ip) ip="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --duration) duration="$2"; shift 2 ;;
    --modes) modes="$2"; shift 2 ;;
    --fps) fps="$2"; shift 2 ;;
    --exposure-ms) exposure_ms="$2"; shift 2 ;;
    --gain-db) gain_db="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
root="$(cd "$(dirname "$0")/../.." && pwd)"
bw="$root/bazel-bin/apps/bandwidth_test/bandwidth_test"
decoder="$root/bazel-bin/tools/py/raw_frame"
[[ -x "$bw" && -x "$decoder" ]] || { echo "build first: bazel build //apps/bandwidth_test //tools/py:raw_frame" >&2; exit 1; }
mkdir -p "$out"
for mode in $modes; do
  dir="$out/$mode"; mkdir -p "$dir"
  cat > "$dir/rig.yaml" <<YAML
hololink_ip: $ip
receiver: $receiver
cuda_device: 0
lane_rate_limit_mbps: 1500
report_interval_s: 2.0
p22:
  enabled: true
  expander_address: 0x20
cameras:
  - {port: $port, mode: $mode, fps: $fps, lanes: 4, exposure_ms: $exposure_ms, gain_db: $gain_db}
YAML
  echo "=== $mode ==="
  "$bw" --config "$dir/rig.yaml" --duration "$duration" --warmup 3 --csv-dir "$dir" --summary "$dir/summary.json" \
        --dump-dir "$dir" --dump-every 30 --dump-limit 3 --crc-every 10 2>&1 | tee "$dir/run.log" | grep -E "PASS|FAIL|aggregate|dumped|Describe|fps=" || true
  shopt -s nullglob
  raws=("$dir"/*.raw)
  if [[ ${#raws[@]} -gt 0 ]]; then
    "$decoder" "${raws[@]}" --out "$dir" --preview-width 1600 --crop 800 > "$dir/decode.json" || true
  fi
done
python3 "$root/tools/capture/catalog.py" "$out" > "$out/catalog.md"
echo "catalogue: $out/catalog.md"
