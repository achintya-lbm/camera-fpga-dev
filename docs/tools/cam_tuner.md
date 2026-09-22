# cam_tuner — live preview, exposure/gain tuning and still capture

`apps/cam_tuner` runs the normal DA322 ingest chain for one or more IMX676 cameras and serves a live
preview with controls on an embedded web page, so the exposure, gain, black level and test pattern can be
changed while looking at the image, and full-resolution stills or raw CSI frames can be captured on demand.

```
receiver → FrameStatsOp → SnapshotOp → CsiToBayerOp → ImageProcessorOp → BayerDemosaicOp
           (fps/Gbps)     (raw dump      (hololink)     (ISP: black level,  (RGBA16)
                          on request)                    auto white balance)   │
                                                                              ├─→ FormatConverterOp (RGB8) → JpegEncoderOp (NPP downscale + nvJPEG) → HTTP server
                                                                              └─→ HolovizOp window (--display, optional)
```

Everything is GPU-side until the JPEG bytes are copied to the host; the stream is rate-limited (10 fps
by default), so the pipeline runs at the sensor's frame rate with no extra load beyond one downscale and one
JPEG per published frame. The implementation of the preview pieces lives in `hsb/preview/`.

## Running it

The tool runs on the machine that hosts the DA322 link (the *test machine*, see `docs/machines.md`); the
web page is opened from any browser on the lab LAN.

```bash
# on the test machine, in the repo checkout
bazel build //apps/cam_tuner
bazel-bin/apps/cam_tuner/cam_tuner --config configs/da322_1cam.yaml --port J1D \
    --mode FULL_RAW10 --fps 30 --exposure-ms 2 --gain-db 0 --receiver linux
```

Then open `http://<test machine>:8080/`. The page shows the MJPEG stream, sliders/inputs for the live
controls, the pipeline statistics, and buttons for **Still** (full-resolution JPEG) and **Capture raw**
(raw CSI frame plus JSON sidecar). Stop with Ctrl-C (or `--duration S`).

Common launch variants:

| Purpose | Command line |
| --- | --- |
| One camera, mode/fps/exposure of your choice | `--config configs/da322_1cam.yaml --port J1D --mode CROP_3552X2160_RAW10 --fps 50 --exposure-ms 3` |
| Every camera in a rig config (one page per camera) | `--config configs/da322_4cam.yaml` → pages on ports 8080, 8081, 8082, 8083 |
| Also a local Holoviz window (needs a display on the test machine) | add `--display` |
| Smaller / faster stream over a slow link | `--stream-width 960 --stream-fps 5 --jpeg-quality 70` |
| RoCE data plane (once the IOMMU fix in `docs/bringup/host_setup.md` §2b is in place) | `--receiver roce` |

`--port` picks one connector (`J1A..J1D` or `CAM1..CAM4`) from the config; if the config does not list it,
the first camera's settings are used with that connector. `--mode`, `--fps`, `--exposure-ms`, `--gain-db`
and `--test-pattern N` override the config for the selected camera(s). Without `--port` all cameras in the
config run and camera *i* gets HTTP port `--http-port + i`.

Full option list: `bazel-bin/apps/cam_tuner/cam_tuner --help`.

## Controls and limits

| Control | Range | Register path |
| --- | --- | --- |
| `exposure_ms` | 0 < t ≤ (VMAX − 8) × 1H (one frame period minus the SHR0 minimum; shown as `exposure_max_ms`) | SHR0 (latched with REGHOLD) |
| `gain_db` | 0 … 72 dB in 0.3 dB steps | GAIN_0 |
| `black_level` | 0 … 1023 (10-bit units; default 50) | BLKLEVEL |
| `test_pattern` / `test_pattern_select` | 0/1, pattern 0 … 31 | TPG_EN_DUOUT / TPG_PATSEL_DUOUT |
| `fps` | 0 < f ≤ mode ceiling at the current lane rate | VMAX (re-derives SHR0 to keep the exposure) |

All changes are applied to the running sensor over the HSB I2C controller; the frame geometry never changes,
so the pipeline keeps running. Out-of-range values are rejected with an error message and leave the sensor
untouched.

## HTTP API

The page is plain HTML/JS on top of these endpoints (all on the camera's port):

| Endpoint | Meaning |
| --- | --- |
| `GET /` | the control page |
| `GET /stream.mjpg` | `multipart/x-mixed-replace` MJPEG stream (`--stream-width`, `--stream-fps`) |
| `GET /snapshot.jpg` | latest stream-size JPEG |
| `GET /still.jpg` | encodes the next frame at full resolution (`--still-quality`), returns it and saves it under `--still-dir` |
| `GET /status.json` | current control state, limits and pipeline statistics (fps, Gbps, frames, gaps, drops, last raw path) |
| `POST /control` | `key=value&...` (form) or a flat JSON object with any of the controls above; `{"ok":true}` or `{"ok":false,"error":...}` |
| `POST /capture` | dumps the next raw CSI frame; returns `{"ok":true,"path":"<base path>"}` |
| `GET /files.json`, `GET /files/<name>` | list / download the files in `--still-dir` |

```bash
curl -s http://<test machine>:8080/status.json
curl -s -X POST -d 'exposure_ms=4&gain_db=6' http://<test machine>:8080/control
curl -s -X POST http://<test machine>:8080/capture
curl -s -o still.jpg http://<test machine>:8080/still.jpg
```

## Stills and raw captures

Files land in `--still-dir` (default `captures/stills`, git-ignored):

- `<camera>_<yyyymmdd-hhmmss>.jpg` — full-resolution JPEG of the demosaiced, ISP-processed frame (what the
  preview shows, at sensor resolution).
- `<camera>_<yyyymmdd-hhmmss>.raw` + `.json` — the raw CSI-2 frame exactly as received (RAW10/RAW12 packed
  lines) and the same sidecar `FrameCheckOp` writes (mode, geometry, bits, line bytes, lane rate, HMAX/VMAX,
  fps). Decode with the raw tool, which writes a preview JPEG, a 1:1 centre crop and statistics:

```bash
bazel build //tools/py:raw_frame
bazel-bin/tools/py/raw_frame captures/stills/cam3-J1D_20260921-181530.raw --out /tmp/decoded --full-png
```

Copy files off the test machine with `scp`, or download them from the page's file list.

## No hardware: emulator loopback

`tools/emulator/loopback.sh` runs the HSB emulator (posing as a DA322 with an emulated IMX676) and the tool
in a private network namespace on one machine:

```bash
bazel build //apps/emu_source //apps/cam_tuner
tools/emulator/loopback.sh --cameras 1 -- \
    bazel-bin/apps/cam_tuner/cam_tuner --config configs/emulator_loopback.yaml --port J1A --http-port 8090
```

The page is only reachable from inside the namespace (the script's shell), e.g.
`tools/emulator/loopback.sh --cameras 1 -- bash -c 'cam_tuner ... & sleep 10; curl -s localhost:8090/status.json; wait'`.
The emulator does not model the sensor's exposure or gain, so control changes are accepted but do not
change the synthetic image.

## Notes

- The Linux receiver (`--receiver linux`) handles every IMX676 mode at its frame-rate ceiling (docs/hardware/imx676_samples.md);
  RoCE needs the IOMMU change in `docs/bringup/host_setup.md`.
- `--display` renders the RGBA16 frames in a Holoviz window on the machine running the tool; the controls stay on the
  web page (Holoviz's ImGui is private to `libholoscan_viz.so`, so there are no in-window sliders).
- `cam_player` is the plain multi-camera viewer without controls; `bandwidth_test` is the throughput/CRC tool.
- `tools/preview/stream_probe.py http://<host>:8080/stream.mjpg 5` records the MJPEG stream for a few seconds and
  reports parts, all-black frames, decode errors and part intervals (exit 1 on any problem); it is how the
  2026-09-21 flicker was pinned down.
- GPU memory: the rig allocates 6 CSI blocks and 4 RGBA16 blocks per camera (`CameraChainOptions::csi_pool_blocks` /
  `bayer_pool_blocks`), 555 MB per camera at 3552×3556. With fewer blocks CsiToBayerOp throws
  `Too many chunks allocated` as soon as the demosaic stage lags (first frames compile NVRTC kernels) instead of
  letting the receiver drop frames.
- `latency` in the stats log and `latency_ms_mean` in `status.json` compare the FPGA's PTP timestamp with the host
  clock; without a PTP master on the camera link the log prints `n/a (no PTP sync)` and the JSON value stays 0.
- Exposure defaults: 2 ms / 0 dB suits the backlit chart used for `docs/hardware/imx676_samples.md`; a normally lit
  room needs about 20 ms / 12 dB. Change them on the page or with `POST /control`.
