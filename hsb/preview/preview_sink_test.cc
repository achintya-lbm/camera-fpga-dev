#include "hsb/preview/preview_sink.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace hsb::preview {
namespace {

EncodedFrame Frame(uint8_t marker) {
  EncodedFrame f;
  f.jpeg = {0xFF, 0xD8, marker, 0xFF, 0xD9};
  f.width = 4;
  f.height = 2;
  return f;
}

TEST(PreviewSink, EmptyUntilPublished) {
  PreviewSink sink;
  EXPECT_FALSE(sink.Latest().has_value());
  EXPECT_EQ(sink.frames_published(), 0u);
  EXPECT_FALSE(sink.WaitForNewer(0, std::chrono::milliseconds(10)).has_value());
}

TEST(PreviewSink, PublishAssignsIncreasingSequences) {
  PreviewSink sink;
  sink.Publish(Frame(1));
  sink.Publish(Frame(2));
  const auto latest = sink.Latest();
  ASSERT_TRUE(latest.has_value());
  EXPECT_EQ(latest->sequence, 2u);
  EXPECT_EQ(latest->jpeg[2], 2);
  EXPECT_EQ(sink.frames_published(), 2u);
  EXPECT_NE(latest->time, std::chrono::steady_clock::time_point{});
}

TEST(PreviewSink, WaitForNewerReturnsOnlyNewerFrames) {
  PreviewSink sink;
  sink.Publish(Frame(1));
  EXPECT_TRUE(sink.WaitForNewer(0, std::chrono::milliseconds(10)).has_value());
  EXPECT_FALSE(sink.WaitForNewer(1, std::chrono::milliseconds(10)).has_value());
  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sink.Publish(Frame(2));
  });
  const auto got = sink.WaitForNewer(1, std::chrono::milliseconds(2000));
  producer.join();
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->sequence, 2u);
}

TEST(PreviewSink, StillRequestIsConsumedOnce) {
  PreviewSink sink;
  EXPECT_FALSE(sink.TakeStillRequest());
  sink.RequestStill();
  sink.RequestStill();
  EXPECT_TRUE(sink.TakeStillRequest());
  EXPECT_FALSE(sink.TakeStillRequest());
}

TEST(PreviewSink, StillsHaveTheirOwnSequence) {
  PreviewSink sink;
  sink.Publish(Frame(1));
  sink.Publish(Frame(2));
  EXPECT_FALSE(sink.LatestStill().has_value());
  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sink.PublishStill(Frame(9));
  });
  const auto still = sink.WaitForStill(0, std::chrono::milliseconds(2000));
  producer.join();
  ASSERT_TRUE(still.has_value());
  EXPECT_EQ(still->sequence, 1u);
  EXPECT_EQ(still->jpeg[2], 9);
  EXPECT_EQ(sink.frames_published(), 2u);  // stills are not counted as stream frames
  EXPECT_FALSE(sink.WaitForStill(1, std::chrono::milliseconds(10)).has_value());
}

TEST(PreviewSink, ManyReadersOneWriter) {
  // Latest-only semantics: every reader sees a strictly increasing subsequence ending at the last frame.
  PreviewSink sink;
  std::atomic<int> received{0};
  std::atomic<int> finished_at_last{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      uint64_t last = 0;
      while (last < 50) {
        auto f = sink.WaitForNewer(last, std::chrono::milliseconds(1000));
        if (!f) break;
        EXPECT_GT(f->sequence, last);
        last = f->sequence;
        received++;
      }
      if (last == 50) finished_at_last++;
    });
  }
  for (int i = 0; i < 50; ++i) {
    sink.Publish(Frame(static_cast<uint8_t>(i)));
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  for (auto& t : readers) t.join();
  EXPECT_GE(received.load(), 4);
  EXPECT_EQ(finished_at_last.load(), 4);
  EXPECT_EQ(sink.frames_published(), 50u);
}

}  // namespace
}  // namespace hsb::preview
