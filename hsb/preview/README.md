# hsb/preview — live preview and control (used by apps/cam_tuner)

Turns the demosaiced RGB frame of one camera into a browser-viewable MJPEG stream with live sensor
controls, full-resolution JPEG stills and raw CSI dumps. No third-party dependencies beyond what the
rest of the tree already uses (Holoscan, CUDA nvJPEG/NPP, fmt).

| File | What it is |
| --- | --- |
| `preview_sink.hpp/.cc` | Thread-safe hand-off between the pipeline and the HTTP threads: one "latest" stream JPEG (sequence numbered, condition-variable wake-up) plus on-demand full-resolution stills. |
| `controls.hpp` | `Controls` interface the application implements (exposure, gain, black level, test pattern, raw capture, status JSON). |
| `jpeg_encoder.hpp/.cc` | `JpegEncoder`: nvJPEG 4:2:0 encode of an interleaved RGB8 device image, optional NPP bilinear downscale, own CUDA stream, reused scratch buffer. Also used by the GPU unit test. |
| `jpeg_encoder_op.hpp/.cc` | `JpegEncoderOp`: Holoscan operator taking a `uint8 [H,W,3]` device tensor (FormatConverterOp `rgb888` output), publishing stream frames at `stream_fps_limit` (extra frames are skipped, never queued) and full-resolution stills when the sink has a pending request. |
| `snapshot_op.hpp/.cc` | `SnapshotOp`: pass-through placed right behind the receiver; `Request()` makes the next compute copy the raw CSI frame to pinned host memory and write `<dump_dir>/<camera>_<YYYYmmdd-HHMMSS-mmm>.raw` + `.json` (sidecar fields as `FrameCheckOp`, readable by `//tools/py:raw_frame`). |
| `preview_server.hpp/.cc` | `PreviewServer`: POSIX HTTP/1.1 server, one thread per connection. |
| `preview_page.hpp/.cc` | The self-contained HTML/JS control page served at `/`. |
| `synthetic_frame.hpp/.cu` | Colour bars + moving box test frame (demo and encoder test). |
| `preview_demo.cc` | Camera-less end-to-end demo (see below). |

## HTTP endpoints

| Method / path | Behaviour |
| --- | --- |
| `GET /` | Control page: `<img src="/stream.mjpg">`, sliders + numeric inputs for `exposure_ms`, `gain_db`, `black_level`, a test-pattern checkbox and pattern select, buttons *Still (full-res JPEG)*, *Capture raw*, *Open snapshot*, a status panel polling `/status.json` once a second and the list of files in `still_dir`. Control changes are debounced (150 ms) and POSTed as JSON. |
| `GET /stream.mjpg` | `multipart/x-mixed-replace; boundary=frame`; each part is one stream-size JPEG. Paced by `PreviewServerOptions::stream_fps`. |
| `GET /snapshot.jpg` | Latest stream-size JPEG (waits up to 2 s for the first frame; 504 if none). |
| `GET /still.jpg` | Requests a full-resolution still from the encoder, waits up to 5 s (504 on timeout), saves it as `<still_dir>/<camera>_<timestamp>.jpg` (name in the `X-File` header) and returns it. |
| `GET /status.json` | `ControlState` fields (`camera, mode, width, height, fps, exposure_ms, gain_db, black_level, test_pattern, test_pattern_select, exposure_max_ms, gain_max_db`), `stream_frames`, merged with the keys of `Controls::StatusJson()`. |
| `POST /control` | Body `application/x-www-form-urlencoded` (`exposure_ms=5&gain_db=6`) or a flat JSON object. Calls `Controls::Apply`; `200 {"ok":true}` or `400 {"ok":false,"error":...}`. |
| `POST /capture` | `Controls::CaptureRaw()`; `200 {"ok":true,"path":"<base>"}` or `500` if the result starts with `error:`. |
| `GET /files.json` | `[{"name","size","mtime"}]` for `still_dir`, newest first. |
| `GET /files/<name>` | Serves a file from `still_dir`; names containing `/` or `..` are rejected (400). |

All responses carry `Cache-Control: no-cache`, `Access-Control-Allow-Origin: *` and `Connection: close`.

## Wiring it into an application

```
receiver → SnapshotOp → FrameStatsOp → CsiToBayerOp → ImageProcessorOp → BayerDemosaicOp
         → FormatConverterOp(out_dtype "rgb888") → JpegEncoderOp(sink)
```

```cpp
auto sink = std::make_shared<hsb::preview::PreviewSink>();
auto encoder = make_operator<hsb::preview::JpegEncoderOp>("jpeg", Arg("sink", sink),
    Arg("stream_width", 1280), Arg("stream_fps_limit", 10.0), Arg("quality", 80), Arg("still_quality", 95));
hsb::preview::PreviewServer server(options, sink, controls /* your Controls implementation */);
server.Start();  // options.port == 0 picks an ephemeral port; server.port() reports it
```

`Controls` is called from HTTP threads: implementations must lock around sensor I2C access.

## Demo (no camera needed)

```
bazel run //hsb/preview:preview_demo -- --port 8080 [--duration 30] [--width 1920 --height 1080 --fps 30]
curl -s localhost:8080/status.json
curl -s -o snap.jpg localhost:8080/snapshot.jpg
curl -s -o still.jpg localhost:8080/still.jpg
curl -s -d 'exposure_ms=12.5&gain_db=6' localhost:8080/control
```

The demo draws colour bars with a moving box on the GPU, streams them through `JpegEncoderOp` and
serves them with a `Controls` implementation that only stores the values (`status.json` reports
`controls_applied`). Stills go to `--still-dir` (default `/tmp/preview_demo_stills`).

## Tests

`bazel test //hsb/preview/...` runs `preview_sink_test` (threading/sequence semantics),
`preview_server_test` (real TCP client on an ephemeral port: page, status merge, snapshot, form and
JSON control, capture, one MJPEG boundary, still save/list/download, traversal rejection, 5 s still
timeout) and `jpeg_encoder_test` (tagged `requires-gpu`, `no-sandbox`, `local`: JPEG framing, quality
ordering, downscale geometry). Use `--config=nogpu` on machines without a GPU.

## Notes

* CUDA 13 removed `nppGetStreamContext()`; `JpegEncoder` fills the `NppStreamContext` itself from
  the device attributes (`nppdefs.h`, "Application Managed Stream Context").
* Frames are encoded on the operator's own non-blocking CUDA stream and synchronised before
  publishing, so the sink only ever holds complete JPEG byte vectors.
