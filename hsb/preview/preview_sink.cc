#include "hsb/preview/preview_sink.hpp"

namespace hsb::preview {

void PreviewSink::Publish(EncodedFrame frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frame.sequence = next_sequence_++;
    if (frame.time == std::chrono::steady_clock::time_point{}) frame.time = std::chrono::steady_clock::now();
    latest_ = std::move(frame);
  }
  cv_.notify_all();
}

std::optional<EncodedFrame> PreviewSink::Latest() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_;
}

std::optional<EncodedFrame> PreviewSink::WaitForNewer(uint64_t after_sequence, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout, [&] { return latest_ && latest_->sequence > after_sequence; });
  if (latest_ && latest_->sequence > after_sequence) return latest_;
  return std::nullopt;
}

void PreviewSink::RequestStill() {
  std::lock_guard<std::mutex> lock(mutex_);
  still_requested_ = true;
}

bool PreviewSink::TakeStillRequest() {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool requested = still_requested_;
  still_requested_ = false;
  return requested;
}

void PreviewSink::PublishStill(EncodedFrame frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frame.sequence = next_still_sequence_++;
    if (frame.time == std::chrono::steady_clock::time_point{}) frame.time = std::chrono::steady_clock::now();
    latest_still_ = std::move(frame);
  }
  cv_.notify_all();
}

std::optional<EncodedFrame> PreviewSink::LatestStill() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return latest_still_;
}

std::optional<EncodedFrame> PreviewSink::WaitForStill(uint64_t after_sequence, std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout, [&] { return latest_still_ && latest_still_->sequence > after_sequence; });
  if (latest_still_ && latest_still_->sequence > after_sequence) return latest_still_;
  return std::nullopt;
}

uint64_t PreviewSink::frames_published() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return next_sequence_ - 1;
}

}  // namespace hsb::preview
