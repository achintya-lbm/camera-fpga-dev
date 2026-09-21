// Thread-safe hand-off of encoded frames between the Holoscan pipeline (JpegEncoderOp) and the
// HTTP server threads (PreviewServer). One "latest" stream-size JPEG, plus on-demand full-size stills.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace hsb::preview {

struct EncodedFrame {
  std::vector<uint8_t> jpeg;  // complete JPEG file bytes
  uint64_t sequence = 0;      // monotonically increasing per sink stream
  int width = 0;
  int height = 0;
  std::chrono::steady_clock::time_point time{};
};

class PreviewSink {
 public:
  // --- stream (called by the encoder op at the stream rate) ---
  void Publish(EncodedFrame frame);
  std::optional<EncodedFrame> Latest() const;
  // Blocks until a frame with sequence > after_sequence exists or the timeout expires.
  std::optional<EncodedFrame> WaitForNewer(uint64_t after_sequence, std::chrono::milliseconds timeout) const;

  // --- stills (full-resolution JPEG of the next frame) ---
  void RequestStill();                 // HTTP thread
  bool TakeStillRequest();             // encoder op: true once per request
  void PublishStill(EncodedFrame frame);
  std::optional<EncodedFrame> LatestStill() const;
  std::optional<EncodedFrame> WaitForStill(uint64_t after_sequence, std::chrono::milliseconds timeout) const;

  uint64_t frames_published() const;

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::optional<EncodedFrame> latest_;
  std::optional<EncodedFrame> latest_still_;
  uint64_t next_sequence_ = 1;
  uint64_t next_still_sequence_ = 1;
  bool still_requested_ = false;
};

}  // namespace hsb::preview
