#include "compression/jxs/headers.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "compression/jxs/bitio.hpp"

namespace jxs {
namespace {

Headers Imx676Headers() {
  Headers h;
  h.cap.Set(Capabilities::kStarTetrix);
  h.cap.Set(Capabilities::kCwd);
  h.pih.lcod = 0;
  h.pih.wf = 1776;
  h.pih.hf = 1778;
  h.pih.cw = 0;
  h.pih.hsl = 16;
  h.pih.nc = 4;
  h.pih.bw = 20;
  h.pih.fq = 8;
  h.pih.br = 4;
  h.pih.cpih = 3;
  h.pih.nlx = 5;
  h.pih.nly = 2;
  h.components.assign(4, Component{12, 1, 1});
  h.sd = 1;
  h.cts = Cts{0, 2, 2};
  h.crg = Crg{{0, 32768, 0, 32768}, {0, 0, 32768, 32768}};
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);
  h.weights.resize(layout.num_bands);
  for (int b = 0; b < layout.num_bands; ++b) h.weights[b] = BandWeights{static_cast<uint8_t>(b % 16), static_cast<uint8_t>(b)};
  h.comments.push_back(Com{0x8000, {'d', 'a', '3', '2', '2'}});
  return h;
}

TEST(BitIo, RoundTrip) {
  BitWriter w;
  w.Put(0x5, 3);
  w.Put(0xABCD, 16);
  w.PutBit(1);
  w.AlignToByte();
  w.Put(0x12345678, 32);
  const auto bytes = w.Take();
  ASSERT_EQ(bytes.size(), 7u);
  BitReader r(bytes);
  EXPECT_EQ(r.Bits(3), 0x5u);
  EXPECT_EQ(r.Bits(16), 0xABCDu);
  EXPECT_EQ(r.Bit(), 1u);
  r.AlignToByte();
  EXPECT_EQ(r.byte_offset(), 3u);
  EXPECT_EQ(r.Bits(32), 0x12345678u);
  EXPECT_THROW(r.Bit(), std::runtime_error);
}

TEST(Headers, WriteParseRoundTrip) {
  const Headers h = Imx676Headers();
  const std::vector<uint8_t> bytes = WriteHeaders(h);
  // SOC(2) CAP(2+2+1) PIH(2+26) CDT(2+10) WGT(2+2+2*31) CWD(2+3) CTS(2+4) CRG(2+18) COM(2+4+5)
  EXPECT_EQ(bytes.size(), 2u + 5 + 28 + 12 + 66 + 5 + 6 + 20 + 11);
  EXPECT_EQ(bytes[0], 0xFF);
  EXPECT_EQ(bytes[1], 0x10);
  EXPECT_EQ(bytes[2], 0xFF);
  EXPECT_EQ(bytes[3], 0x50);
  EXPECT_EQ(bytes[6], 0x44);  // CAP bits 1 and 5 -> 0b01000100

  std::vector<uint8_t> stream = bytes;
  WriteSliceHeader(SliceHeader{false, 0}, &stream);
  const ParsedHeaders parsed = ParseHeaders(stream);
  EXPECT_EQ(parsed.first_slice_offset, bytes.size());
  EXPECT_EQ(parsed.headers.pih, h.pih);
  EXPECT_EQ(parsed.headers.components, h.components);
  EXPECT_EQ(parsed.headers.weights, h.weights);
  EXPECT_EQ(parsed.headers.sd, 1);
  EXPECT_EQ(parsed.headers.cap.bits, h.cap.bits);
  ASSERT_TRUE(parsed.headers.cts.has_value());
  EXPECT_EQ(*parsed.headers.cts, *h.cts);
  ASSERT_TRUE(parsed.headers.crg.has_value());
  EXPECT_EQ(*parsed.headers.crg, *h.crg);
  ASSERT_EQ(parsed.headers.comments.size(), 1u);
  EXPECT_EQ(parsed.headers.comments[0], h.comments[0]);
  EXPECT_FALSE(parsed.headers.nlt.has_value());
  EXPECT_FALSE(parsed.headers.tpc.has_value());

  const SliceHeader slice = ParseSliceHeader(stream, parsed.first_slice_offset);
  EXPECT_FALSE(slice.tdc);
  EXPECT_EQ(slice.ysl, 0);
}

TEST(Headers, PictureHeaderBitLayout) {
  Headers h = Imx676Headers();
  h.pih.lcod = 0x01020304;
  h.pih.qpih = 1;
  h.pih.fs = 1;
  h.pih.rm = 1;
  h.pih.lh = 1;
  const auto bytes = WriteHeaders(h);
  // PIH starts after SOC(2) + CAP(2 + 2 + 1 payload byte): marker, length 26, then Lcod.
  const size_t pih = 7;
  EXPECT_EQ(bytes[pih], 0xFF);
  EXPECT_EQ(bytes[pih + 1], 0x12);
  EXPECT_EQ(bytes[pih + 3], 26);
  EXPECT_EQ(bytes[pih + 4], 0x01);
  EXPECT_EQ(bytes[pih + 7], 0x04);
  // Wf at payload offset 8 -> bytes[pih+4+8].
  EXPECT_EQ((bytes[pih + 12] << 8) | bytes[pih + 13], 1776);
  // Nc, Ng, Ss, Bw at payload offsets 16..19.
  EXPECT_EQ(bytes[pih + 20], 4);
  EXPECT_EQ(bytes[pih + 21], 4);
  EXPECT_EQ(bytes[pih + 22], 8);
  EXPECT_EQ(bytes[pih + 23], 20);
  // Fq(4) Br(4) = 0x84; Fslc(1) Ppoc(3) Cpih(4) = 0b0000'0011 = 0x03; NLx(4) NLy(4) = 0x52;
  // Lh(1) Rl(1) Qpih(2) Fs(2) Rm(2) = 0b1'0'01'01'01 = 0x95.
  EXPECT_EQ(bytes[pih + 24], 0x84);
  EXPECT_EQ(bytes[pih + 25], 0x03);
  EXPECT_EQ(bytes[pih + 26], 0x52);
  EXPECT_EQ(bytes[pih + 27], 0x95);
}

TEST(Headers, ValidateRejectsInconsistentStreams) {
  {
    Headers h = Imx676Headers();
    h.cts.reset();
    EXPECT_THROW(Validate(h), std::runtime_error);  // CTS mandatory for Star-Tetrix
  }
  {
    Headers h = Imx676Headers();
    h.cap.Set(Capabilities::kStarTetrix, false);
    EXPECT_THROW(Validate(h), std::runtime_error);
  }
  {
    Headers h = Imx676Headers();
    h.pih.fq = 0;  // lossless requires Bw = B[0] and CAP bit 6
    EXPECT_THROW(Validate(h), std::runtime_error);
    h.pih.bw = 12;
    EXPECT_THROW(Validate(h), std::runtime_error);
    h.cap.Set(Capabilities::kLossless);
    EXPECT_NO_THROW(Validate(h));
  }
  {
    Headers h = Imx676Headers();
    h.pih.cw = 3;  // 8*3*32 = 768; 1776 umod 768 = 240 >= 32 -> allowed
    EXPECT_NO_THROW(Validate(h));
    h.pih.cw = 7;  // 1792 > 1776 -> 1776 umod 1792 = 1776 >= 32 -> allowed but a single column
    EXPECT_NO_THROW(Validate(h));
    h.pih.wf = 1792;
    h.pih.cw = 1;  // 1792 umod 256 = 0 < 32 -> rejected
    EXPECT_THROW(Validate(h), std::runtime_error);
  }
  {
    Headers h = Imx676Headers();
    h.pih.nly = 6;  // > NL,x
    EXPECT_THROW(Validate(h), std::runtime_error);
  }
  {
    Headers h = Imx676Headers();
    h.weights.pop_back();
    EXPECT_THROW(Validate(h), std::runtime_error);
  }
}

TEST(Headers, ParseRejectsBadMarkers) {
  Headers h = Imx676Headers();
  std::vector<uint8_t> bytes = WriteHeaders(h);
  WriteSliceHeader(SliceHeader{false, 0}, &bytes);
  {
    std::vector<uint8_t> broken = bytes;
    broken[1] = 0x11;  // EOC instead of SOC
    EXPECT_THROW(ParseHeaders(broken), std::runtime_error);
  }
  {
    std::vector<uint8_t> broken = bytes;
    broken[2] = 0xFF;
    broken[3] = 0x12;  // PIH where CAP must be
    EXPECT_THROW(ParseHeaders(broken), std::runtime_error);
  }
  {
    std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + 20);
    EXPECT_THROW(ParseHeaders(truncated), std::runtime_error);
  }
}

TEST(Headers, PrecinctAndPacketHeaders) {
  const Headers h = Imx676Headers();
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);
  EXPECT_EQ(layout.num_bands, 31);
  EXPECT_EQ(PrecinctHeaderBytes(layout, false), 13u);  // 24+8+8+62 = 102 bits
  EXPECT_EQ(PrecinctHeaderBytes(layout, true), 23u);   // + 16 + 62 = 180 bits

  PrecinctHeader ph;
  ph.lprc = 0x0ABCDE;
  ph.q = 17;
  ph.r = 5;
  ph.d.assign(31, 0);
  for (int b = 0; b < 31; ++b) ph.d[b] = static_cast<uint8_t>(b % 4);
  std::vector<uint8_t> bytes;
  WritePrecinctHeader(ph, layout, false, &bytes);
  ASSERT_EQ(bytes.size(), 13u);
  const PrecinctHeader back = ParsePrecinctHeader(bytes, 0, layout, false);
  EXPECT_EQ(back.lprc, ph.lprc);
  EXPECT_EQ(back.q, ph.q);
  EXPECT_EQ(back.r, ph.r);
  EXPECT_EQ(back.qf, 32);
  EXPECT_EQ(back.d, ph.d);

  EXPECT_TRUE(UseShortPacketHeader(h.pih));
  EXPECT_EQ(PacketHeaderBytes(h.pih), 5u);
  PacketHeader pk{true, 30000, 8000, 2000};
  std::vector<uint8_t> pk_bytes;
  WritePacketHeader(pk, true, &pk_bytes);
  ASSERT_EQ(pk_bytes.size(), 5u);
  const PacketHeader pk_back = ParsePacketHeader(pk_bytes, 0, true);
  EXPECT_EQ(pk_back.dr, true);
  EXPECT_EQ(pk_back.ldat, 30000u);
  EXPECT_EQ(pk_back.lcnt, 8000u);
  EXPECT_EQ(pk_back.lsgn, 2000u);
  PacketHeader too_big{false, 40000, 0, 0};
  EXPECT_THROW(WritePacketHeader(too_big, true, &pk_bytes), std::runtime_error);
  WritePacketHeader(too_big, false, &pk_bytes);
  EXPECT_EQ(pk_bytes.size(), 5u + 7u);
  EXPECT_EQ(ParsePacketHeader(pk_bytes, 5, false).ldat, 40000u);
}

}  // namespace
}  // namespace jxs
