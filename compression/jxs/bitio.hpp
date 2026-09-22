// Bit-level reader/writer for JPEG XS codestreams (ISO/IEC 21122-1 A.1.2: bit strings are
// consumed left bit first, numeric fields MSB first).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace jxs {

class BitReader {
 public:
  explicit BitReader(std::span<const uint8_t> data, size_t byte_offset = 0) : data_(data), pos_(byte_offset * 8) {}

  // Reads `n` bits (0 <= n <= 32) MSB first.
  uint32_t Bits(int n) {
    if (n < 0 || n > 32) throw std::invalid_argument("BitReader::Bits: n out of range");
    if (pos_ + static_cast<size_t>(n) > data_.size() * 8) throw std::runtime_error("BitReader: read past end of data");
    uint32_t value = 0;
    for (int i = 0; i < n; ++i) {
      value = (value << 1) | Bit();
    }
    return value;
  }
  uint32_t Bit() {
    if (pos_ >= data_.size() * 8) throw std::runtime_error("BitReader: read past end of data");
    const uint32_t bit = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u;
    ++pos_;
    return bit;
  }
  // Skips to the next byte boundary (pad(8) in the standard's pseudo-code).
  void AlignToByte() { pos_ = (pos_ + 7) & ~static_cast<size_t>(7); }
  size_t bit_position() const { return pos_; }
  size_t byte_offset() const { return pos_ >> 3; }  // valid after AlignToByte()
  bool byte_aligned() const { return (pos_ & 7) == 0; }
  size_t bits_remaining() const { return data_.size() * 8 - pos_; }
  void SeekByte(size_t byte_offset) {
    if (byte_offset > data_.size()) throw std::runtime_error("BitReader: seek past end of data");
    pos_ = byte_offset * 8;
  }
  std::span<const uint8_t> data() const { return data_; }

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};

class BitWriter {
 public:
  void Put(uint64_t value, int n) {
    if (n < 0 || n > 64) throw std::invalid_argument("BitWriter::Put: n out of range");
    if (n < 64 && (value >> n) != 0) throw std::invalid_argument("BitWriter::Put: value does not fit");
    for (int i = n - 1; i >= 0; --i) PutBit(static_cast<uint32_t>((value >> i) & 1u));
  }
  void PutBit(uint32_t bit) {
    if ((bit_count_ & 7) == 0) bytes_.push_back(0);
    if (bit) bytes_.back() |= static_cast<uint8_t>(0x80u >> (bit_count_ & 7));
    ++bit_count_;
  }
  // Zero-pads to the next byte boundary.
  void AlignToByte() { bit_count_ = (bit_count_ + 7) & ~static_cast<size_t>(7); }
  void PutBytes(std::span<const uint8_t> bytes) {
    AlignToByte();
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
    bit_count_ += bytes.size() * 8;
  }
  size_t bit_count() const { return bit_count_; }
  size_t byte_count() const { return (bit_count_ + 7) / 8; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  std::vector<uint8_t> Take() {
    bit_count_ = 0;
    return std::move(bytes_);
  }

 private:
  std::vector<uint8_t> bytes_;
  size_t bit_count_ = 0;
};

}  // namespace jxs
