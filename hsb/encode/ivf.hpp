// IVF container (DKIF) writer/reader for AV1 elementary streams.
// Layout: 32-byte file header, then per frame a 12-byte header (u32 size, u64 pts)
// followed by the frame payload (temporal unit). Little-endian throughout.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hsb::encode {

struct IvfHeader {
  char fourcc[4] = {'A', 'V', '0', '1'};
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t timebase_num = 30;  // frame rate numerator ("rate")
  uint32_t timebase_den = 1;   // frame rate denominator ("scale")
  uint32_t frame_count = 0;
};

class IvfWriter {
 public:
  IvfWriter() = default;
  ~IvfWriter();
  IvfWriter(const IvfWriter&) = delete;
  IvfWriter& operator=(const IvfWriter&) = delete;

  // Creates/truncates `path` and writes the header (frame_count is patched on Close()).
  bool Open(const std::string& path, const IvfHeader& header);
  bool WriteFrame(const uint8_t* data, std::size_t size, uint64_t pts);
  // Patches the frame count and closes the file. Safe to call twice.
  bool Close();
  bool is_open() const { return file_ != nullptr; }
  uint32_t frames_written() const { return frames_; }

 private:
  std::FILE* file_ = nullptr;
  uint32_t frames_ = 0;
};

struct IvfFrame {
  uint64_t pts = 0;
  std::vector<uint8_t> data;
};

class IvfReader {
 public:
  bool Open(const std::string& path);
  const IvfHeader& header() const { return header_; }
  // Returns false at end of file or on a truncated frame.
  bool ReadFrame(IvfFrame& frame);
  void Close();
  ~IvfReader() { Close(); }

 private:
  std::FILE* file_ = nullptr;
  IvfHeader header_{};
};

// Serialises the 32-byte header; exposed for unit tests.
void SerializeIvfHeader(const IvfHeader& header, uint8_t out[32]);
bool ParseIvfHeader(const uint8_t in[32], IvfHeader& header);

}  // namespace hsb::encode
