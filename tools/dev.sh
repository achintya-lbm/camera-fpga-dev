#!/usr/bin/env bash
# Run Bazel (and everything else) inside the dev container.
#
#   tools/dev.sh up|down|image|shell
#   tools/dev.sh build|test|run|query|cquery|fetch|mod|clean|info [bazel args...]
#   tools/dev.sh exec <command...>
#
# The container is long-lived (Bazel server stays warm) and mounts this repo at
# /workspace plus a named volume at /var/cache/bazel for outputs and caches.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${DEV_IMAGE:-camera-fpga-dev:dev}"
BASE_IMAGE="${DEV_BASE_IMAGE:-nvcr.io/nvidia/clara-holoscan/holoscan:v3.9.0-cuda13}"
NAME="${DEV_CONTAINER:-camera-fpga-dev}"
CACHE_VOL="${DEV_CACHE_VOLUME:-camera-fpga-dev-bazel-cache}"

image() {
  docker build --build-arg "BASE_IMAGE=${BASE_IMAGE}" -t "${IMAGE}" \
    -f "${ROOT}/tools/docker/Dockerfile.dev" "${ROOT}/tools/docker"
}

running() {
  [ "$(docker inspect -f '{{.State.Running}}' "${NAME}" 2>/dev/null || true)" = "true" ]
}

up() {
  if running; then return 0; fi
  docker rm -f "${NAME}" >/dev/null 2>&1 || true
  docker image inspect "${IMAGE}" >/dev/null 2>&1 || image

  local -a extra=()
  if command -v nvidia-smi >/dev/null 2>&1; then
    extra+=(--gpus all --runtime nvidia)
  fi
  if [ -d /dev/infiniband ]; then
    extra+=(--device /dev/infiniband)   # ibverbs for the RoCE receiver
  fi
  if [ -d /tmp/.X11-unix ]; then
    extra+=(-v /tmp/.X11-unix:/tmp/.X11-unix:ro -e "DISPLAY=${DISPLAY:-}")
  fi

  docker run -d --name "${NAME}" \
    --net host --ipc host \
    --ulimit memlock=-1 --ulimit stack=67108864 \
    --cap-add IPC_LOCK --cap-add SYS_NICE --cap-add SYS_PTRACE \
    --security-opt seccomp=unconfined \
    -e NVIDIA_DRIVER_CAPABILITIES=all \
    -e HOME=/var/cache/bazel/home \
    -e BAZELISK_HOME=/var/cache/bazel/bazelisk \
    -u "$(id -u):$(id -g)" \
    -v /etc/passwd:/etc/passwd:ro -v /etc/group:/etc/group:ro \
    -v "${ROOT}:/workspace" \
    -v "${CACHE_VOL}:/var/cache/bazel" \
    -w /workspace \
    "${extra[@]}" \
    "${IMAGE}" sleep infinity >/dev/null

  # The named volume is root-owned when first created.
  docker exec -u 0 "${NAME}" chown "$(id -u):$(id -g)" /var/cache/bazel
  docker exec "${NAME}" mkdir -p /var/cache/bazel/home /var/cache/bazel/bazelisk
}

down() {
  docker rm -f "${NAME}" >/dev/null 2>&1 || true
}

dexec() {
  up
  local -a tty=(-i)
  [ -t 0 ] && [ -t 1 ] && tty=(-it)
  docker exec "${tty[@]}" -w /workspace "${NAME}" "$@"
}

bazel_in_container() {
  dexec bazel --bazelrc=/workspace/tools/docker/container.bazelrc "$@"
}

cmd="${1:-help}"
shift || true
case "${cmd}" in
  up) up ;;
  down) down ;;
  image) image ;;
  shell) dexec bash ;;
  exec) dexec "$@" ;;
  build|test|run|query|cquery|aquery|fetch|mod|clean|info|shutdown|version)
    bazel_in_container "${cmd}" "$@" ;;
  bazel) bazel_in_container "$@" ;;
  *)
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1 ;;
esac
