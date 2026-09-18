# third_party

| Repo | Source | Notes |
|---|---|---|
| `@holoscan_sdk` | `/opt/nvidia/holoscan` inside the dev container (`new_local_repository`) | Wrapped by `holoscan/holoscan.BUILD`. Version follows the container tag in `tools/dev.sh`. |
| `@nv_codec_headers` | FFmpeg/nv-codec-headers `n13.0.19.1` (MIT) | NVENC API 13.0 headers; runtime libraries come from the NVIDIA driver via dlopen. |
| `@hololink` (M2) | holoscan-sensor-bridge @ `6930609` (2.5.0-PB6) + `hololink/patches/` | Vendor patch imported; Bazel overlay is milestone M2. |

Rules: use the SDK's own copies of fmt/spdlog/yaml-cpp (do not add competing `bazel_dep`s);
keep machine specifics out of here (see `docs/machines.md`).
