#include "compression/jxs/geometry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace jxs {
namespace {

// Headers for `nc` 4:4:4 components (plus `sd` non-decomposed ones) at the IMX676 super-pixel size.
Headers MakeHeaders(int nc, int nlx, int nly, int sd = 0, int cpih = 0) {
  Headers h;
  h.pih.wf = 1776;
  h.pih.hf = 1778;
  h.pih.nc = static_cast<uint8_t>(nc);
  h.pih.nlx = static_cast<uint8_t>(nlx);
  h.pih.nly = static_cast<uint8_t>(nly);
  h.pih.hsl = 16;
  h.pih.cpih = static_cast<uint8_t>(cpih);
  h.components.assign(nc, Component{12, 1, 1});
  h.sd = static_cast<uint8_t>(sd);
  if (sd > 0) h.cap.Set(Capabilities::kCwd);
  if (cpih == 3) {
    h.cap.Set(Capabilities::kStarTetrix);
    h.cts = Cts{0, 2, 2};
    h.crg = Crg{{0, 32768, 0, 32768}, {0, 0, 32768, 32768}};
  }
  const BandLayout layout = ComputeBandLayout(h.pih, h.components, h.sd);
  h.weights.assign(layout.num_bands, BandWeights{});
  return h;
}

std::string BandName(const Band& b) {
  if (!b.decomposed) return "raw";
  std::string s;
  s += b.high_pass_x ? 'H' : 'L';
  s += b.high_pass_y ? 'H' : 'L';
  return s + std::to_string(b.dx) + "," + std::to_string(b.dy);
}

// Tables B.1-B.3: filter types for 5 horizontal levels and 0, 1, 2 vertical levels.
TEST(Geometry, FilterTypesMatchTablesB1ToB3) {
  {
    Geometry g(MakeHeaders(1, 5, 0));
    const std::vector<std::string> expected = {"LL5,0", "HL5,0", "HL4,0", "HL3,0", "HL2,0", "HL1,0"};
    ASSERT_EQ(g.n_beta(), 6);
    for (int beta = 0; beta < 6; ++beta) EXPECT_EQ(BandName(g.band(g.band_index(beta, 0))), expected[beta]) << beta;
  }
  {
    Geometry g(MakeHeaders(1, 5, 1));
    const std::vector<std::string> expected = {"LL5,1", "HL5,1", "HL4,1", "HL3,1", "HL2,1", "HL1,1", "LH1,1", "HH1,1"};
    ASSERT_EQ(g.n_beta(), 8);
    for (int beta = 0; beta < 8; ++beta) EXPECT_EQ(BandName(g.band(g.band_index(beta, 0))), expected[beta]) << beta;
  }
  {
    Geometry g(MakeHeaders(1, 5, 2));
    const std::vector<std::string> expected = {"LL5,2", "HL5,2", "HL4,2", "HL3,2", "HL2,2", "LH2,2", "HH2,2", "HL1,1", "LH1,1", "HH1,1"};
    ASSERT_EQ(g.n_beta(), 10);
    for (int beta = 0; beta < 10; ++beta) EXPECT_EQ(BandName(g.band(g.band_index(beta, 0))), expected[beta]) << beta;
  }
}

// B.3 NOTE 8: band index = (Nc - Sd) * beta + i, component fastest; non-decomposed components after.
TEST(Geometry, BandIndicesAndCounts) {
  Geometry g(MakeHeaders(4, 5, 2, /*sd=*/1, /*cpih=*/3));
  EXPECT_EQ(g.n_beta(), 10);
  EXPECT_EQ(g.num_bands(), 31);
  EXPECT_EQ(g.band_index(0, 0), 0);
  EXPECT_EQ(g.band_index(0, 2), 2);
  EXPECT_EQ(g.band_index(1, 0), 3);
  EXPECT_EQ(g.band_index(9, 2), 29);
  EXPECT_EQ(g.band_index(0, 3), 30);
  EXPECT_EQ(g.band_index(1, 3), -1);
  EXPECT_FALSE(g.band(30).decomposed);
  EXPECT_EQ(g.band(30).width, 1776);
  EXPECT_EQ(g.band(30).height, 1778);
  for (const Band& b : g.bands()) EXPECT_TRUE(b.exists) << b.index;
  // LL5,2 of component 0: ceil(1776/32) x ceil(1778/4).
  EXPECT_EQ(g.band(0).width, 56);
  EXPECT_EQ(g.band(0).height, 445);
  // HH1,1: floor(ceil(1776/1)/2) x floor(ceil(1778/1)/2).
  EXPECT_EQ(g.band(g.band_index(9, 0)).width, 888);
  EXPECT_EQ(g.band(g.band_index(9, 0)).height, 889);
}

// Appendix B of the notes / B.5: precinct grid of the IMX676 stream.
TEST(Geometry, PrecinctGridForImx676) {
  Headers h = MakeHeaders(4, 5, 2, 1, 3);
  Geometry g(h);
  EXPECT_EQ(g.precinct_lines(), 4);
  EXPECT_EQ(g.precincts_per_row(), 1);
  EXPECT_EQ(g.precinct_rows(), 445);
  EXPECT_EQ(g.column_width(), 1776);
  EXPECT_EQ(g.precinct_width(0), 1776);
  EXPECT_EQ(g.num_slices(), 28);
  EXPECT_EQ(g.slice_precinct_rows(0), 16);
  EXPECT_EQ(g.slice_precinct_rows(27), 445 - 27 * 16);
  EXPECT_EQ(g.slice_first_precinct(1), 16);
  EXPECT_TRUE(g.short_packet_header());  // 1776 * 4 = 7104 < 32752

  // Cw = 2: 512-wide columns, 4 per line, last one 240 wide.
  h.pih.cw = 2;
  Geometry g2(h);
  EXPECT_EQ(g2.column_width(), 512);
  EXPECT_EQ(g2.precincts_per_row(), 4);
  EXPECT_EQ(g2.precinct_width(0), 512);
  EXPECT_EQ(g2.precinct_width(3), 240);
  // Wpb: LL5,2 in a 512 column = 512/32 = 16; in the last column ceil(240/32) = 8.
  EXPECT_EQ(g2.band_precinct_width(0, 0), 16);
  EXPECT_EQ(g2.band_precinct_width(3, 0), 8);
  EXPECT_EQ(g2.code_groups(0, 0), 4);
  EXPECT_EQ(g2.significance_groups(0, 0), 1);
  // HH1,1 in a 512 column: floor(ceil(512/1)/2) = 256 -> 64 code groups, 8 significance groups.
  const int hh11 = g2.band_index(9, 0);
  EXPECT_EQ(g2.band_precinct_width(0, hh11), 256);
  EXPECT_EQ(g2.code_groups(0, hh11), 64);
  EXPECT_EQ(g2.significance_groups(0, hh11), 8);
}

// B.6 / Figure B.3: lines of each band within a 4-line precinct.
TEST(Geometry, LinesWithinPrecinct) {
  Geometry g(MakeHeaders(4, 5, 2, 1, 3));
  const int p = 0;
  EXPECT_EQ(g.line_start(g.band_index(0, 0)), 0);  // LL5,2
  EXPECT_EQ(g.line_end(p, g.band_index(0, 0)), 1);
  EXPECT_EQ(g.line_start(g.band_index(5, 0)), 1);  // LH2,2
  EXPECT_EQ(g.line_end(p, g.band_index(5, 0)), 2);
  EXPECT_EQ(g.line_start(g.band_index(7, 0)), 0);  // HL1,1: lines 0,1
  EXPECT_EQ(g.line_end(p, g.band_index(7, 0)), 2);
  EXPECT_EQ(g.line_start(g.band_index(8, 0)), 2);  // LH1,1: lines 2,3
  EXPECT_EQ(g.line_end(p, g.band_index(8, 0)), 4);
  EXPECT_EQ(g.line_start(30), 0);  // non-decomposed: lines 0..3
  EXPECT_EQ(g.line_end(p, 30), 4);

  // Bottom precinct row (row 444) has 2 sampling-grid lines: 1778 - 444*4.
  const int last = g.num_precincts() - 1;
  EXPECT_EQ(g.line_end(last, 30), 2);
  EXPECT_EQ(g.line_end(last, g.band_index(0, 0)), 1);   // LL: 445 lines total, one per row
  EXPECT_EQ(g.line_end(last, g.band_index(5, 0)), 1);   // LH2,2: 444 lines -> none in the last row (L1 == L0)
  EXPECT_EQ(g.line_end(last, g.band_index(7, 0)), 1);   // HL1,1: 889 lines -> 1 in the last row
  EXPECT_EQ(g.line_end(last, g.band_index(8, 0)), 3);   // LH1,1: 889 lines -> 1 line (lambda 2)
}

std::vector<std::pair<int, std::vector<int>>> Flatten(const std::vector<Packet>& packets) {
  std::vector<std::pair<int, std::vector<int>>> out;
  for (const Packet& pk : packets) {
    std::vector<int> bands;
    for (const auto& e : pk.entries) bands.push_back(e.band);
    out.push_back({pk.entries.front().line, bands});
  }
  return out;
}

// Table B.5: 5h/0v, three 4:4:4 components -> one packet per precinct.
TEST(Geometry, PacketsTableB5) {
  Geometry g(MakeHeaders(3, 5, 0));
  const auto packets = Flatten(g.packets(0));
  ASSERT_EQ(packets.size(), 1u);
  EXPECT_EQ(packets[0].first, 0);
  std::vector<int> all;
  for (int b = 0; b < 18; ++b) all.push_back(b);
  EXPECT_EQ(packets[0].second, all);
}

// Table B.6: 5h/1v, three components.
TEST(Geometry, PacketsTableB6) {
  Geometry g(MakeHeaders(3, 5, 1));
  const auto packets = Flatten(g.packets(0));
  ASSERT_EQ(packets.size(), 4u);
  std::vector<int> first;
  for (int b = 0; b < 15; ++b) first.push_back(b);
  EXPECT_EQ(packets[0], (std::pair<int, std::vector<int>>{0, first}));
  EXPECT_EQ(packets[1], (std::pair<int, std::vector<int>>{0, {15, 16, 17}}));
  EXPECT_EQ(packets[2], (std::pair<int, std::vector<int>>{1, {18, 19, 20}}));
  EXPECT_EQ(packets[3], (std::pair<int, std::vector<int>>{1, {21, 22, 23}}));
}

// Table B.7: 5h/2v, three components.
TEST(Geometry, PacketsTableB7) {
  Geometry g(MakeHeaders(3, 5, 2));
  const auto packets = Flatten(g.packets(0));
  ASSERT_EQ(packets.size(), 10u);
  std::vector<int> first;
  for (int b = 0; b < 12; ++b) first.push_back(b);
  using P = std::pair<int, std::vector<int>>;
  EXPECT_EQ(packets[0], (P{0, first}));
  EXPECT_EQ(packets[1], (P{0, {12, 13, 14}}));
  EXPECT_EQ(packets[2], (P{1, {15, 16, 17}}));
  EXPECT_EQ(packets[3], (P{1, {18, 19, 20}}));
  EXPECT_EQ(packets[4], (P{0, {21, 22, 23}}));
  EXPECT_EQ(packets[5], (P{2, {24, 25, 26}}));
  EXPECT_EQ(packets[6], (P{2, {27, 28, 29}}));
  EXPECT_EQ(packets[7], (P{1, {21, 22, 23}}));
  EXPECT_EQ(packets[8], (P{3, {24, 25, 26}}));
  EXPECT_EQ(packets[9], (P{3, {27, 28, 29}}));
}

// Table B.10: 5h/1v, four components with Sd = 1.
TEST(Geometry, PacketsTableB10) {
  Geometry g(MakeHeaders(4, 5, 1, 1, 3));
  const auto packets = Flatten(g.packets(0));
  ASSERT_EQ(packets.size(), 6u);
  using P = std::pair<int, std::vector<int>>;
  EXPECT_EQ(packets[1], (P{0, {15, 16, 17}}));
  EXPECT_EQ(packets[2], (P{1, {18, 19, 20}}));
  EXPECT_EQ(packets[3], (P{1, {21, 22, 23}}));
  EXPECT_EQ(packets[4], (P{0, {24}}));
  EXPECT_EQ(packets[5], (P{1, {24}}));
}

// Table B.11: 5h/2v, four components with Sd = 1 (the CFA layout of Annex I).
TEST(Geometry, PacketsTableB11) {
  Geometry g(MakeHeaders(4, 5, 2, 1, 3));
  const auto packets = Flatten(g.packets(0));
  ASSERT_EQ(packets.size(), 14u);
  using P = std::pair<int, std::vector<int>>;
  EXPECT_EQ(packets[9], (P{3, {27, 28, 29}}));
  EXPECT_EQ(packets[10], (P{0, {30}}));
  EXPECT_EQ(packets[11], (P{1, {30}}));
  EXPECT_EQ(packets[12], (P{2, {30}}));
  EXPECT_EQ(packets[13], (P{3, {30}}));

  // Bottom precinct row: LH2,2/HH2,2 vanish, only lambda 0 of HL1,1 and lambda 2 of LH1,1/HH1,1 remain,
  // and the raw component has two lines.
  const auto last = Flatten(g.packets(g.num_precincts() - 1));
  std::vector<P> expected = {
      P{0, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}}, P{0, {12, 13, 14}}, P{0, {21, 22, 23}}, P{2, {24, 25, 26}},
      P{2, {27, 28, 29}}, P{0, {30}}, P{1, {30}},
  };
  EXPECT_EQ(last, expected);
}

TEST(Geometry, RejectsVerticalSubsampling) {
  Headers h = MakeHeaders(3, 5, 1);
  h.components[1] = Component{8, 2, 2};
  h.components[2] = Component{8, 2, 2};
  h.cap.Set(Capabilities::kVerticalSubsampling);
  h.weights.assign(ComputeBandLayout(h.pih, h.components, 0).num_bands, BandWeights{});
  EXPECT_THROW(Geometry g(h), std::runtime_error);
}

TEST(Geometry, SubsampledChroma422) {
  Headers h = MakeHeaders(3, 5, 2);
  h.components[1].sx = 2;
  h.components[2].sx = 2;
  Geometry g(h);
  EXPECT_EQ(g.component_width(1), 888);
  EXPECT_EQ(g.band(g.band_index(0, 1)).width, 28);  // ceil(888/32)
  h.pih.cw = 1;                                      // 8*1*2*32 = 512
  Geometry g2(h);
  EXPECT_EQ(g2.column_width(), 512);
  EXPECT_EQ(g2.band_precinct_width(0, g2.band_index(0, 1)), 8);  // ceil(512/(2*32))
}

}  // namespace
}  // namespace jxs
