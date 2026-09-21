// Minimal HTTP server (POSIX sockets, one thread per connection, no third-party deps) that serves the
// live preview and control page:
//   GET  /               HTML control page (embedded), shows the MJPEG stream, sliders/inputs for the
//                        controls, status, and buttons for "still" (full-res JPEG) and "capture raw"
//   GET  /stream.mjpg    multipart/x-mixed-replace MJPEG of the stream-size frames
//   GET  /snapshot.jpg   latest stream-size JPEG
//   GET  /still.jpg      requests a full-resolution still, waits for it (<= 5 s), returns it; also
//                        saved under still_dir as <camera>_<timestamp>.jpg
//   GET  /status.json    Controls::StatusJson() merged with ControlState as JSON
//   POST /control        application/x-www-form-urlencoded or JSON body of key=value pairs -> Controls::Apply
//   POST /capture        Controls::CaptureRaw(); returns {"path": ...}
//   GET  /files/<name>   files from still_dir (download of stills/raws)
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hsb/preview/controls.hpp"
#include "hsb/preview/preview_sink.hpp"

namespace hsb::preview {

struct PreviewServerOptions {
  std::string bind_address = "0.0.0.0";
  uint16_t port = 8080;
  std::string title = "cam_tuner";
  std::string still_dir = "captures/stills";
  double stream_fps = 10.0;  // pacing hint for the MJPEG loop (frames arrive at this rate at most)
};

class PreviewServer {
 public:
  PreviewServer(PreviewServerOptions options, std::shared_ptr<PreviewSink> sink, std::shared_ptr<Controls> controls);
  ~PreviewServer();
  PreviewServer(const PreviewServer&) = delete;
  PreviewServer& operator=(const PreviewServer&) = delete;

  void Start();  // binds and listens; throws std::runtime_error on failure
  void Stop();   // closes the listening socket and joins the threads
  uint16_t port() const { return options_.port; }

 private:
  struct Impl;
  void HandleConnection(int fd);

  PreviewServerOptions options_;
  std::shared_ptr<PreviewSink> sink_;
  std::shared_ptr<Controls> controls_;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hsb::preview
