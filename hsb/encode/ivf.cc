#include "hsb/encode/ivf.hpp"

#include <cstring>

namespace hsb::encode {
namespace {

void PutLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void PutLe32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void PutLe64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t GetLe16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t GetLe32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}
uint64_t GetLe64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kFrameHeaderSize = 12;

}  // namespace

void SerializeIvfHeader(const IvfHeader& h, uint8_t out[32]) {
  std::memset(out, 0, kHeaderSize);
  std::memcpy(out, "DKIF", 4);
  PutLe16(out + 4, 0);                        // version
  PutLe16(out + 6, kHeaderSize);              // header size
  std::memcpy(out + 8, h.fourcc, 4);
  PutLe16(out + 12, h.width);
  PutLe16(out + 14, h.height);
  PutLe32(out + 16, h.timebase_num);
  PutLe32(out + 20, h.timebase_den);
  PutLe32(out + 24, h.frame_count);
  PutLe32(out + 28, 0);                       // unused
}

bool ParseIvfHeader(const uint8_t in[32], IvfHeader& h) {
  if (std::memcmp(in, "DKIF", 4) != 0) return false;
  if (GetLe16(in + 4) != 0 || GetLe16(in + 6) != kHeaderSize) return false;
  std::memcpy(h.fourcc, in + 8, 4);
  h.width = GetLe16(in + 12);
  h.height = GetLe16(in + 14);
  h.timebase_num = GetLe32(in + 16);
  h.timebase_den = GetLe32(in + 20);
  h.frame_count = GetLe32(in + 24);
  return true;
}

IvfWriter::~IvfWriter() { Close(); }

bool IvfWriter::Open(const std::string& path, const IvfHeader& header) {
  Close();
  file_ = std::fopen(path.c_str(), "wb");
  if (!file_) return false;
  uint8_t buf[kHeaderSize];
  SerializeIvfHeader(header, buf);
  frames_ = 0;
  return std::fwrite(buf, 1, kHeaderSize, file_) == kHeaderSize;
}

bool IvfWriter::WriteFrame(const uint8_t* data, std::size_t size, uint64_t pts) {
  if (!file_ || size > 0xFFFFFFFFu) return false;
  uint8_t hdr[kFrameHeaderSize];
  PutLe32(hdr, static_cast<uint32_t>(size));
  PutLe64(hdr + 4, pts);
  if (std::fwrite(hdr, 1, kFrameHeaderSize, file_) != kFrameHeaderSize) return false;
  if (size && std::fwrite(data, 1, size, file_) != size) return false;
  ++frames_;
  return true;
}

bool IvfWriter::Close() {
  if (!file_) return true;
  bool ok = true;
  uint8_t count[4];
  PutLe32(count, frames_);
  if (std::fseek(file_, 24, SEEK_SET) != 0 || std::fwrite(count, 1, 4, file_) != 4) ok = false;
  if (std::fclose(file_) != 0) ok = false;
  file_ = nullptr;
  return ok;
}

bool IvfReader::Open(const std::string& path) {
  Close();
  file_ = std::fopen(path.c_str(), "rb");
  if (!file_) return false;
  uint8_t buf[kHeaderSize];
  if (std::fread(buf, 1, kHeaderSize, file_) != kHeaderSize || !ParseIvfHeader(buf, header_)) {
    Close();
    return false;
  }
  return true;
}

bool IvfReader::ReadFrame(IvfFrame& frame) {
  if (!file_) return false;
  uint8_t hdr[kFrameHeaderSize];
  if (std::fread(hdr, 1, kFrameHeaderSize, file_) != kFrameHeaderSize) return false;
  const uint32_t size = GetLe32(hdr);
  frame.pts = GetLe64(hdr + 4);
  frame.data.resize(size);
  return size == 0 || std::fread(frame.data.data(), 1, size, file_) == size;
}

void IvfReader::Close() {
  if (file_) std::fclose(file_);
  file_ = nullptr;
}

}  // namespace hsb::encode
