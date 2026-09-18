#include "hsb/encode/ivf.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace hsb::encode {
namespace {

std::string TempPath(const char* name) {
  const char* dir = std::getenv("TEST_TMPDIR");
  return std::string(dir ? dir : "/tmp") + "/" + name;
}

TEST(Ivf, HeaderRoundTrip) {
  IvfHeader h;
  h.width = 3552;
  h.height = 3552;
  h.timebase_num = 40;
  h.timebase_den = 1;
  h.frame_count = 1234;
  uint8_t buf[32];
  SerializeIvfHeader(h, buf);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(buf), 4), "DKIF");
  EXPECT_EQ(buf[6], 32);  // header size, little-endian low byte
  IvfHeader parsed;
  ASSERT_TRUE(ParseIvfHeader(buf, parsed));
  EXPECT_EQ(std::string(parsed.fourcc, 4), "AV01");
  EXPECT_EQ(parsed.width, 3552);
  EXPECT_EQ(parsed.height, 3552);
  EXPECT_EQ(parsed.timebase_num, 40u);
  EXPECT_EQ(parsed.timebase_den, 1u);
  EXPECT_EQ(parsed.frame_count, 1234u);
}

TEST(Ivf, RejectsBadMagic) {
  uint8_t buf[32] = {'R', 'I', 'F', 'F'};
  IvfHeader parsed;
  EXPECT_FALSE(ParseIvfHeader(buf, parsed));
}

TEST(Ivf, WriteThenReadFrames) {
  const std::string path = TempPath("frames.ivf");
  IvfHeader h;
  h.width = 640;
  h.height = 360;
  {
    IvfWriter w;
    ASSERT_TRUE(w.Open(path, h));
    const uint8_t a[] = {0x12, 0x00, 0x0a, 0x0b};
    const uint8_t b[] = {0x32, 0x01, 0x02};
    ASSERT_TRUE(w.WriteFrame(a, sizeof a, 1000));
    ASSERT_TRUE(w.WriteFrame(b, sizeof b, 2000));
    ASSERT_TRUE(w.WriteFrame(nullptr, 0, 3000));  // empty frame allowed
    EXPECT_EQ(w.frames_written(), 3u);
    ASSERT_TRUE(w.Close());
  }
  IvfReader r;
  ASSERT_TRUE(r.Open(path));
  EXPECT_EQ(r.header().frame_count, 3u);  // patched on Close()
  EXPECT_EQ(r.header().width, 640);
  IvfFrame f;
  ASSERT_TRUE(r.ReadFrame(f));
  EXPECT_EQ(f.pts, 1000u);
  ASSERT_EQ(f.data.size(), 4u);
  EXPECT_EQ(f.data[0], 0x12);
  ASSERT_TRUE(r.ReadFrame(f));
  EXPECT_EQ(f.pts, 2000u);
  EXPECT_EQ(f.data.size(), 3u);
  ASSERT_TRUE(r.ReadFrame(f));
  EXPECT_EQ(f.pts, 3000u);
  EXPECT_TRUE(f.data.empty());
  EXPECT_FALSE(r.ReadFrame(f));  // EOF
}

}  // namespace
}  // namespace hsb::encode
