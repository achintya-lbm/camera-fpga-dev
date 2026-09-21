#!/usr/bin/env bash
# No-FPGA loopback: runs apps/emu_source (posing as a DA322) and a receiver-side app on the same
# machine inside a private user+network namespace. The emulator's Linux data plane needs a raw
# IP socket (CAP_NET_RAW); an unprivileged user namespace grants that without sudo, and its own
# loopback keeps the HSB ports off the real network. CUDA devices stay accessible.
#
#   tools/emulator/loopback.sh [--cameras N] [--emu-args "..."] -- <receiver command and args>
#
#   tools/emulator/loopback.sh --cameras 2 -- \
#       bazel-bin/apps/bandwidth_test/bandwidth_test --config configs/emulator_loopback.yaml --duration 15
set -euo pipefail

cameras=1
emu_args=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --cameras) cameras="$2"; shift 2 ;;
    --emu-args) emu_args="$2"; shift 2 ;;
    --) shift; break ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
done
if [[ $# -eq 0 ]]; then
  echo "usage: $0 [--cameras N] [--emu-args \"...\"] -- <receiver command>" >&2
  exit 2
fi

root="$(cd "$(dirname "$0")/../.." && pwd)"
emu="$root/bazel-bin/apps/emu_source/emu_source"
if [[ ! -x "$emu" ]]; then
  echo "build first: bazel build //apps/emu_source //apps/bandwidth_test //apps/cam_player" >&2
  exit 1
fi

export LOOPBACK_EMU="$emu" LOOPBACK_CAMERAS="$cameras" LOOPBACK_EMU_ARGS="$emu_args"
exec unshare -Urn bash -c '
  set -u
  ip link set lo up
  "$LOOPBACK_EMU" --ip 127.0.0.1 --cameras "$LOOPBACK_CAMERAS" $LOOPBACK_EMU_ARGS &
  emu_pid=$!
  sleep 1.5
  "$@"
  status=$?
  kill -INT "$emu_pid" 2>/dev/null
  wait "$emu_pid" 2>/dev/null
  exit $status
' loopback "$@"
