# JPEG XS (ISO/IEC 21122) implementation landscape

Survey date: 2026-09-22. Purpose: pick an oracle for a home-grown CUDA decoder and a
Lattice CertusPro-NX FPGA encoder for raw Bayer frames from Sony IMX676 sensors
(3552 x 3556, 10/12-bit RGGB, ~33 fps).

Method: every claim below carries a URL. Pages were fetched directly; where a page
could not be fetched or a number could not be read out of a chart, the item is
marked **unverified**. For SVT-JPEG-XS and the ISO reference software the actual
source trees were downloaded and grepped (SVT-JPEG-XS `main` at commit `f9c82c1`,
2026-09-18; ISO/IEC 21122-5 ed. 3 zip dated 2025-07-21), so statements about them
are about the code, not the marketing.

Not covered here: repo integration. Nothing in this document was built in the
repo; the Bazel/CMake notes in section 1.1.9 are recipes, not tested targets.

---

## 0. One-paragraph summary

There is exactly one permissively licensed, production-grade JPEG XS codec
(Intel's SVT-JPEG-XS, BSD-2-Clause-Patent), and it does **not** encode the Bayer
profiles: its encoder never signals Star-Tetrix/CFA, NLT, component-dependent
decomposition or lossless, so Bayer/TDC/MLS profiles are explicitly "out of
scope"; its decoder, however, does implement the inverse Star-Tetrix (Cpih=3) and
the NLTs, so it can decode Bayer-profile streams. The only open implementation
that encodes *and* decodes every profile including LightBayer/MainBayer/HighBayer
is the ISO/IEC 21122-5 reference software (`libjxs`, 3rd edition, 2025), which is
freely downloadable from standards.iso.org but licensed by intoPIX/Fraunhofer/Canon
only for "evaluation" and "conformance testing" of your own implementation, with a
RAND commitment for anything else. The ISO/IEC 21122-4 conformance package (3rd
edition) is also freely downloadable (1.85 GB, 186 codestreams) and contains seven
Bayer-profile codestreams plus a Python codestream dumper. Commercial FPGA IP for
Lattice exists (intoPIX TicoXS, ported to CertusPro-NX among others) but publishes
no LUT numbers; the only public FPGA footprint numbers found are from the
IHSE/Fraunhofer core (Cyclone 10 GX / UltraScale+), which is not Bayer-capable yet.
Recommendation (section 5): use the ISO reference software as the *primary* oracle
for Bayer-profile codestreams and the ISO conformance streams as fixed test
vectors; use SVT-JPEG-XS as the *secondary* decoder oracle (it decodes Bayer
streams) and as the fast non-Bayer path; do not link the reference software into
anything shipped.

---

## 1. Open-source software implementations

### 1.1 Intel / Open Visual Cloud SVT-JPEG-XS

Repository: https://github.com/OpenVisualCloud/SVT-JPEG-XS (94 stars, language C,
default branch `main`, last push 2026-09-22 per the GitHub API
https://api.github.com/repos/OpenVisualCloud/SVT-JPEG-XS).

#### 1.1.1 License

`LICENSE.md` is the **BSD+Patent** licence, SPDX `BSD-2-Clause-Patent`, copyright
2024 Intel Corporation, with an express patent grant from each contributor for the
contribution "alone or in combination with the original work" -
https://raw.githubusercontent.com/OpenVisualCloud/SVT-JPEG-XS/main/LICENSE.md.
GitHub's licence detector reports `NOASSERTION` because the file has a preamble,
but the SPDX identifier is stated in the file itself and in every source header
(e.g. `Source/Lib/Encoder/Codec/ProfileLevel.h`). Note the patent grant covers
Intel's/contributors' patents only; the JPEG XS standard-essential patents held by
intoPIX and Fraunhofer are **not** covered (see section 2.6).

#### 1.1.2 Language, build system, platforms

- Language: C (library), C++ only for gtest unit tests. Assembly kernels in
  NASM/YASM syntax. https://raw.githubusercontent.com/OpenVisualCloud/SVT-JPEG-XS/main/CMakeLists.txt
- Build: CMake. `CMakeLists.txt` says `cmake_minimum_required` 3.10, project
  `svt-jpegxs` version **0.10.0**; the README says "CMake 3.16 or later".
  Requires NASM >= 2.14 or YASM (option `ENABLE_NASM`, default prefers YASM);
  64-bit only. Options: `BUILD_APPS` (default ON), `BUILD_TESTING` (gtest 1.14.0
  vendored in `third_party/`), `BUILD_SHARED_LIBS` (default ON), `NATIVE`
  (`-march=native`), `JPEGXS_LTO` (default ON for GCC>=9/Clang>=12/MSVC),
  `JPEGXS_PGO`, `COVERAGE`, `SANITIZER`. Headers install to
  `${includedir}/svt-jpegxs`; a pkg-config file `SvtJpegxs.pc` is generated from
  `Source/Lib/pkg-config.pc.in` (made relocatable in Aug 2026). Same URL as above.
- Convenience scripts: `Build/linux/build.sh <release|debug> [install --prefix ..]`,
  `Build/windows/build.bat <2022|2019>`. https://github.com/OpenVisualCloud/SVT-JPEG-XS#build-and-install
- Platforms stated: Linux (Ubuntu 20.04/22.04), Windows 10/11 64-bit; a "Macos
  build support" commit landed 2026-08-11 (GitHub API commit list). **x86-64
  only** - no ARM/NEON code paths exist in the tree (the CPU-flags enum is
  MMX..AVX-512/BMI2 only, `Source/API/SvtJpegxs.h`).
- Releases: a single tag `v0.9.0` (2024-03-27) exists
  (https://github.com/OpenVisualCloud/SVT-JPEG-XS/tags); the tree on `main`
  identifies itself as 0.10.0 / API 0.10 and has been very active through
  Aug-Sep 2026 (AVX-512 GCLI vectorisation, alpha formats, log callback,
  MSB-aligned samples, LTO/PGO). Pin a commit, not the tag, if you want the
  current features.

#### 1.1.3 CPU SIMD requirements

Runtime dispatch over C, MMX, SSE, SSE2, SSE3, SSSE3, SSE4.1, SSE4.2, AVX, AVX2,
AVX-512 (and BMI2 fast paths added 2026-09-09); selectable with `--asm` / the
`use_cpu_flags` API field. Unit tests require AVX2 to run. README:
https://github.com/OpenVisualCloud/SVT-JPEG-XS#readme and
`Source/API/SvtJpegxs.h` (CPU_FLAGS enum).

#### 1.1.4 Supported profiles / levels / sublevels

From `Source/Lib/Encoder/Codec/ProfileLevel.h`
(https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/Source/Lib/Encoder/Codec/ProfileLevel.h),
quoted verbatim because it is the single most important fact for this project:

> Only the "Main" family is auto-derivable by derive_codestream_profile_ppih()
> below: this encoder never signals Star-Tetrix/RAW-CFA, non-linear transforms,
> component-dependent decomposition or lossless coding (see
> write_capabilities_marker()), so Bayer/TDC/MLS profiles are out of scope, and
> Light/High family membership additionally implies encoder-complexity guarantees
> that cannot be safely inferred from configuration alone. Light/High/other values
> are only reachable via the profile_override_enable/profile_ppih_override API
> fields.

- Ppih values the encoder knows: Light 422.10 (0x1500), Light 444.12 (0x1A00),
  Light-Subline 422.10 (0x2500), Main 420.12 (0x3240), Main 422.10 (0x3540),
  Main 444.12 (0x3A40), Main 4444.12 (0x3E40), High 420.12 (0x4240), High 444.12
  (0x4A40), High 4444.12 (0x4E40). Auto-derivation always picks a Main profile;
  13/14-bit input is "outside every defined ISO/IEC 21122-2 lossy profile" and
  gets the closest Main profile plus a warning.
- Levels: Unrestricted, 1k-1, 2k-1, 4k-1/2/3, 5k-1, 8k-1/2/3, 10k-1; sublevels
  Unrestricted/12/9/6/4/3/2 bpp; FBB-level bits always 0 (no TDC).
- CAP marker written by the encoder (`Source/Lib/Encoder/Codec/PackHeaders.c`):
  only bit 4 (4:2:0 present) and bit 8 (raw-mode per packet, `Rl`) can be set;
  bits 1 (Star-Tetrix/CTS), 2/3 (NLT), 5 (component-dependent decomposition), 6
  (lossless) are hard-coded 0. A `--cap-compat` / "legacy decoder" mode emits an
  empty CAP marker (Lcap=2) because "some strict decoders reject a non-empty CAP
  marker".
- The **decoder** accepts Cpih 0 (none), 1 (RCT) and **3 (Star-Tetrix)**
  (`Source/Lib/Decoder/Codec/ParseHeader.c`), requires CTS and CRG markers when
  Cpih=3, implements `inverse_star_tetrix()` and `inverse_rct()`
  (`Source/Lib/Decoder/Codec/Mct.c`, with CFA pattern derived from the CRG marker
  per Table F.9) and the inverse quadratic/extended NLTs
  (`Source/Lib/Decoder/Codec/NltDec.h`, `hdr_Tnlt*` fields in
  `Source/Lib/Common/Codec/Pi.h`). `pi_t.Sd` ("Number components with suppressed
  decomposition ... Support for Bayer format") is also parsed. The decoder design
  doc mentions "RGB->YUV or Star-Tetrix" as the supported colour transforms:
  https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/documentation/decoder/svt-jpegxs-decoder-design.md.
  Whether a Bayer-profile stream from the ISO conformance set decodes bit-exactly
  in SVT was **not tested here** (unverified); the unit test
  `tests/UnitTests/TestInvDwtFrame.cc` does exercise `mct_sizes = {0, 1, 3}`
  ("0-disable, 1-rct, 3-star-tetrix").
- No TDC, no MLS/lossless, no 16-bit: grep of the tree finds no TDC/temporal code
  and lossless only as the CAP bit comment.

#### 1.1.5 Bit depths and colour formats (encoder input / decoder output)

README table (https://github.com/OpenVisualCloud/SVT-JPEG-XS#supported-colour-formats):
planar YUV 4:2:0 / 4:2:2 / 4:4:4 and planar RGB (`gbrp`) at 8/10/12/14-bit
("Tested, working properly"); YUV 4:2:2 + alpha (4:2:2:4) and RGBA/YUVA444
(4:4:4:4) at 8/10/12/14-bit (added 2026-08-06); packed RGB 8/10/12/14-bit
(decoder outputs planar only). **YUV 4:0:0 8-bit and 10/12/14-bit are listed as
"Unsupported"** for the encoder, although `COLOUR_FORMAT_PLANAR_YUV400` and
`COLOUR_FORMAT_GRAY` exist in the enum and the decoder maps 1-component streams
to `COLOUR_FORMAT_GRAY` (`Source/Lib/Decoder/Codec/Decoder.c`).

`ColourFormat_t` (`Source/API/SvtJpegxs.h`,
https://raw.githubusercontent.com/OpenVisualCloud/SVT-JPEG-XS/main/Source/API/SvtJpegxs.h):
`COLOUR_FORMAT_INVALID`, `PLANAR_YUV400`, `PLANAR_YUV420`, `PLANAR_YUV422`,
`PLANAR_YUV444_OR_RGB`, `PLANAR_4_COMPONENTS` ("planar 4 components, 4:4:4:4
sampling (RGBA, GBRA, YUVA444 etc.)"), `GRAY`, `PLANAR_YUV422_ALPHA`,
`PACKED_YUV444_OR_RGB`.

Sample alignment: 10/12-bit samples are LSB-aligned in `uint16_t` by default;
`input_bit_depth_msb_aligned` / `output_bit_depth_msb_aligned` (added 2026-08-04)
select an MSB-aligned host-side convention (`Source/API/SvtJpegxsEnc.h`,
`SvtJpegxsDec.h`).

#### 1.1.6 Bayer / CFA input: what you can and cannot do with SVT today

- You **cannot** produce an ISO Bayer-profile (Ppih 0x9300/0xB340/0xC340,
  Cpih=3) codestream with the SVT encoder: no API field sets Cpih, CTS, CRG, NLT
  or Sd (`Source/API/SvtJpegxsEnc.h` has none of them), and the CAP bits are
  hard-wired to 0 (section 1.1.4).
- You **can** encode an RGGB frame as four quarter-resolution planes using
  `COLOUR_FORMAT_PLANAR_4_COMPONENTS` (EncApp `--colour-format rgba`/`yuva444`
  + `--input-depth 10|12`), which yields a **Main 4444.12** codestream (Cpih=0,
  no decorrelation between the four planes). That is a legal JPEG XS stream any
  conformant decoder handles, at the cost of the Star-Tetrix coding gain (section
  4) and of the 12-bit cap of the 4444.12 profiles (Bayer profiles allow 10-16
  bit, https://en.wikipedia.org/wiki/JPEG_XS). This is a home-made layout, not
  the standard's Bayer coding.
- The SVT **decoder** does decode Cpih=3 + NLT streams (section 1.1.4), so it can
  serve as a second decoder for Bayer-profile streams produced by the ISO
  reference encoder or by the FPGA.

#### 1.1.7 Encoder rate-control and coding options

From `Source/API/SvtJpegxsEnc.h`
(https://raw.githubusercontent.com/OpenVisualCloud/SVT-JPEG-XS/main/Source/API/SvtJpegxsEnc.h)
and the README option list:

| API field / CLI | Meaning |
|---|---|
| `bpp_numerator`, `bpp_denominator` / `--bpp` | target bits per pixel as a fraction (float accepted on CLI) |
| `ndecomp_v` (0-2, default 2), `ndecomp_h` (1-5, default 5) / `--decomp_v`, `--decomp_h` | wavelet decomposition levels; `decomp_h >= decomp_v` |
| `quantization` / `--quantization` | 0 = deadzone (default), 1 = uniform |
| `slice_height` / `--slice-height` | default 16 lines, multiple of 2^decomp_v |
| `rate_control_mode` / `--rc` | 0 CBR budget per precinct; 1 per precinct with padding movement (CLI default); 2 CBR budget per slice; 3 per slice with max-rate |
| `coding_signs_handling` / `--coding-signs` | 0 disable, 1 fast, 2 full |
| `coding_significance` / `--coding-sigf` | significance coding on/off |
| `coding_vertical_prediction_mode` / `--coding-vpred` | 0 disable, 1/2 enable, 3/4 force |
| `--coding-raw` | raw-mode per packet (`Rl`) |
| `profile_ppih_override`, `level_plev_override` / `--stream-profile`, `--stream-level` | write Ppih/Plev verbatim instead of auto-derive |
| `cpu_profile` / `--profile` | 0 low latency (default), 1 low CPU usage |
| `threads_num` / `--lp`, `use_cpu_flags` / `--asm`, `--packetization-mode`, `--limit-fps` | threading / ISA / packet vs frame output / latency measurement |

Rate allocation (encoder design doc,
https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/documentation/encoder/svt-jpegxs-encoder-design.md):
per precinct, search the best quantization (GTLI truncation) with refinement 0,
then search the best refinement; budget is per precinct or per slice according to
`rate_control_mode`.

#### 1.1.8 Library API (public headers and entry points)

Headers in `Source/API/`: `SvtJpegxs.h` (common types, `svt_jpeg_xs_image_config_t`,
`svt_jpeg_xs_image_buffer_t`, `svt_jpeg_xs_bitstream_buffer_t`,
`svt_jpeg_xs_frame_t`, error codes, `svt_jpeg_xs_set_log_callback()`),
`SvtJpegxsEnc.h`, `SvtJpegxsDec.h`. API version macros
`SVT_JPEGXS_API_VER_MAJOR 0`, `SVT_JPEGXS_API_VER_MINOR 10`.

Encoder (`SvtJpegxsEnc.h`): `svt_jpeg_xs_encoder_load_default_parameters()`,
`svt_jpeg_xs_encoder_get_image_config()`, `svt_jpeg_xs_encoder_init()`,
`svt_jpeg_xs_encoder_send_picture(enc, frame, blocking)`,
`svt_jpeg_xs_encoder_get_packet(enc, frame, blocking)`, `svt_jpeg_xs_encoder_close()`.

Decoder (`SvtJpegxsDec.h`,
https://raw.githubusercontent.com/OpenVisualCloud/SVT-JPEG-XS/main/Source/API/SvtJpegxsDec.h):
`svt_jpeg_xs_decoder_init(major, minor, dec, bitstream, size, &image_config)`,
`svt_jpeg_xs_decoder_send_frame()` (frame mode) or `svt_jpeg_xs_decoder_send_packet()`
(packet mode, `packetization_mode = 1`), `svt_jpeg_xs_decoder_get_frame()`,
`svt_jpeg_xs_decoder_get_single_frame_size[_with_proxy]()`,
`svt_jpeg_xs_decoder_send_eoc()`, `svt_jpeg_xs_decoder_close()`; `proxy_mode`
full/half/quarter decodes a downscaled picture. Snippets:
https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/documentation/encoder/EncoderSnippets.md
and `.../decoder/DecoderSnippets.md`. Sample apps: `Source/App/SampleEncoder`,
`Source/App/SampleDecoder`.

#### 1.1.9 Throughput

Intel publishes **no** fps numbers in the README or docs (checked; the README only
explains how to measure latency with `--limit-fps` and `--lp`). Phoronix Test
Suite has a `pts/svt-jpeg-xs` profile ("SVT-JPEG-XS 0.9, Input: Bosphorus 4K",
"scales well with increasing CPU core counts") but the results page returned HTTP
403 to this survey, so no figures: https://openbenchmarking.org/test/pts/svt-jpeg-xs
(**unverified**). The in-tree `tests/scripts/PerformanceTest*.sh` scripts exist
but ship no reference numbers.

#### 1.1.10 Building from a tarball with CMake or Bazel

Tarball: `https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/<commit-or-tag>.tar.gz`
(tag pattern `refs/tags/v0.9.0.tar.gz`, https://github.com/OpenVisualCloud/SVT-JPEG-XS/tags).

CMake (from the ffmpeg-plugin readme, which is the maintainers' own static-lib
recipe, https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/ffmpeg-plugin/readme.md):

```
cmake -S . -B build -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX
cmake --build build -j && cmake --install build
# -> $PREFIX/include/svt-jpegxs/{SvtJpegxs.h,SvtJpegxsEnc.h,SvtJpegxsDec.h}, $PREFIX/lib/libSvtJpegxs.a, SvtJpegxs.pc
```

Bazel: the practical route is `rules_foreign_cc`'s `cmake()` rule
(module `rules_foreign_cc`, latest 0.16.0 on the BCR,
https://registry.bazel.build/modules/rules_foreign_cc; rule attributes
`lib_source`, `cache_entries`, `out_static_libs`, `out_include_dir`,
`generate_args`, `build_args`, `install`, `env`, per
https://raw.githubusercontent.com/bazel-contrib/rules_foreign_cc/main/docs/README.md).
Sketch (untested):

```
# MODULE.bazel
bazel_dep(name = "rules_foreign_cc", version = "0.16.0")
http_archive = use_repo_rule("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
http_archive(name = "svt_jpegxs_src",
    urls = ["https://github.com/OpenVisualCloud/SVT-JPEG-XS/archive/<sha>.tar.gz"],
    strip_prefix = "SVT-JPEG-XS-<sha>",
    build_file_content = 'filegroup(name="all", srcs=glob(["**"]), visibility=["//visibility:public"])')

# BUILD
load("@rules_foreign_cc//foreign_cc:defs.bzl", "cmake")
cmake(name = "svt_jpegxs", lib_source = "@svt_jpegxs_src//:all",
    cache_entries = {"BUILD_APPS": "OFF", "BUILD_TESTING": "OFF", "BUILD_SHARED_LIBS": "OFF",
                     "CMAKE_BUILD_TYPE": "Release", "JPEGXS_LTO": "OFF"},
    out_include_dir = "include/svt-jpegxs", out_static_libs = ["libSvtJpegxs.a"])
```

Caveats: NASM or YASM must be on the host (or declared as a toolchain); LTO should
be off when linking into a non-LTO Bazel binary; `-march=native` (`NATIVE`) must
stay off for reproducible builds. A hand-written `cc_library` is possible but
means driving `nasm` for the `.asm` kernels yourself; not recommended.

#### 1.1.11 Test suite (what could be reused as an oracle)

Tree: https://github.com/OpenVisualCloud/SVT-JPEG-XS/tree/main/tests and
https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/documentation/tests/Readme.md.

- `tests/UnitTests/*.cc` (gtest): DWT/IDWT, quant/dequant, pack/unpack, GC stage,
  NLT (`TestNlt.cc`), MSB alignment, rate control, profile/level derivation,
  four-component alpha, decoder API, and a decoder round trip on a single
  embedded 16x16 8-bit 4:2:2 codestream (`SampleFramesData.cc`). No external
  vectors; no image-quality tests.
- `tests/scripts/DecoderConformanceTest.sh` decodes 60 external files
  `$INPUT_FILES_PATH/test_bitsreams/NNN.jxs` and byte-compares against
  `reference_decode/*.yuv`. The names (e.g. `002 4064x2704_8bit_YUV422`,
  `019 4096x1743_13bit_COMPONENTS_4`, `028 4073x1744_8bit_UNKNOWN_GRAY`,
  `010 11328x2704_11bit_YUV422`) match the dimensions of ISO/IEC 21122-4
  codestreams 1-67 (section 3.1), i.e. the script is a harness for the ISO
  conformance set, **but the vectors are not in the repository** ("Set of sample
  files should be downloaded to server", `documentation/tests/Readme.md`).
- `tests/FuzzyTests` (libFuzzer harnesses), `tests/scripts/OomifyTest.sh`,
  sanitizer scripts; FFmpeg and GStreamer plugin functional/performance scripts.
- `Build/linux/PGO/*.yuv` are small training clips (720p/1080p/4K, 8/10/12-bit),
  not references.

### 1.2 FFmpeg (libavcodec via libsvtjpegxs)

- Upstream FFmpeg merged a JPEG-XS parser, decoder and encoder wrappers around
  `libsvtjpegxs`, a raw JPEG-XS muxer/demuxer, and JPEG-XS in MPEG-TS in
  December 2025 (https://www.phoronix.com/news/FFmpeg-Merges-JPEG-XS,
  2025-12-14). The `Changelog` lists "JPEG-XS parser", "JPEG-XS decoder and
  encoder through libsvtjpegxs", "JPEG-XS raw bitstream muxer and demuxer" under
  **version 8.1** (https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/Changelog);
  FFmpeg 8.1 "Hoare" was released 2026-03-16 (https://ffmpeg.org/index.html).
- `libavcodec/libsvtjpegxsenc.c` (LGPL-2.1+ / BSD-2-Clause-Patent dual header)
  exposes options `decomp_v`, `decomp_h`, `quantization`, `coding-signs`,
  `coding-sigf`, `coding-vpred`, and accepts only planar YUV 4:2:0/4:2:2/4:4:4 at
  8/10/12/14-bit (https://ffmpeg.org/doxygen/trunk/libsvtjpegxsenc_8c_source.html);
  no RGB, 4-component or gray pix_fmts, so it cannot be used for a 4-plane Bayer
  layout. Decoder: https://ffmpeg.org/doxygen/trunk/libsvtjpegxsdec_8c.html.
- SVT-JPEG-XS ships patch sets for FFmpeg 6.1/7.0/7.1/8.0/8.1/9.0 in
  `ffmpeg-plugin/` (https://github.com/OpenVisualCloud/SVT-JPEG-XS/blob/main/ffmpeg-plugin/readme.md)
  and, since Aug 2026, an out-of-tree YUVA422/YUVA444/GBRA 4-component mapping
  (GitHub API commit list) that is not in upstream 8.1.
- intoPIX sells an FFmpeg integration of its FastTicoXS SDK (https://www.intopix.com/ffmpeg).

### 1.3 GStreamer

`svtjpegxsenc` / `svtjpegxsdec` (gst-plugins-bad) landed in **GStreamer 1.26.0
(2025-03-11)** together with JPEG XS in the MPEG-TS muxer/demuxer; the plugin is
shipped in the binary releases (https://gstreamer.freedesktop.org/releases/1.26/).
Encoder properties: `bits-per-pixel` (double, default 3), `quantization-mode`,
`decomposition-h/-v`, `slice-height`, `rate-control-mode`, `threads`; caps are
YCbCr only: `Y444, Y42B, I420` and their `_10LE/_12LE` variants, 16x16 to
16384x16384 (https://gstreamer.freedesktop.org/documentation/svtjpegxs/svtjpegxsenc.html,
https://gstreamer.freedesktop.org/documentation/svtjpegxs/svtjpegxsdec.html). No
gray or 4-component caps, so again no Bayer path. MSYS2 packages `svt-jpeg-xs`
(https://packages.msys2.org/packages/mingw-w64-x86_64-svt-jpeg-xs).

### 1.4 ISO/IEC 21122-5 reference software ("jxs" / `libjxs`)

- Status: ISO/IEC 21122-5:2025 is the 3rd edition (stage 60.60), per the JPEG XS
  workplan https://jpeg.org/jpegxs/workplan.html and https://www.iso.org/standard/89031.html.
  jpeg.org's software page lists it as the *only* software and links directly to
  the zip: https://jpeg.org/jpegxs/software.html ->
  https://standards.iso.org/iso-iec/21122/-5/ed-3/en/ISO_IEC_21122-5_ED-3.zip
  (198,298 bytes, Last-Modified 2025-07-21; `curl` fetched it with **no login**,
  only the ISO Customer Licence click-through on the HTML index
  https://standards.iso.org/iso-iec/21122/-5/ed-3/en/). Older: ed-1 index
  https://standards.iso.org/iso-iec/21122/-5/ed-1/en/ (`SC_29_21122-5_att.zip`).
- **License** (`LICENSE.md` in the zip, quoted): "intoPIX SA, Fraunhofer IIS and
  Canon Inc ... grants, to any implementer of this ISO Standard, an irrevocable,
  non-exclusive, worldwide, royalty-free, sub-licensable copyright licence to
  prepare derivative works of ... the Software ... for the following limited
  purposes: (i) to evaluate the Software and any derivative works thereof for
  inclusion in its implementation of this ISO Standard, and (ii) to determine
  whether its implementation conforms with this ISO Standard." Then: "IN THE
  EVENT YOU WISH TO INCLUDE THE SOFTWARE IN A CONFORMING IMPLEMENTATION ... The
  Software Copyright Holder agrees to grant a copyright license on reasonable and
  non-discriminatory terms" and, twice, "No patent licence is granted". So: fine
  as a test oracle and for studying the algorithm, **not** redistributable inside
  a product without a separate RAND copyright licence. Not OSI open source.
- Contents (73 files, C): `libjxs/public/libjxs.h` + `libjxs/src/{bio,dwt,ids,
  image,mct,nlt,packing,precinct,precinct_budget_table,predbuffer,quant,
  rate_control,sb_weighting,xs_config,xs_config_parser,xs_dec,xs_enc,xs_markers}.c`,
  and `programs/` (`jxs_encoder`, `jxs_decoder`, converters for PGX/PPM/PGM/
  planar/mono/v210/yuv16/rgb16/uyvy8/argb). CMake >= 3.12; "GCC (9.3) and Visual
  Studio (16.9)"; Linux/Windows/macOS (README in zip).
- Profile coverage is complete: `xs_profile_e` has Light/Main/High 4:2:0..4:4:4:4,
  MLS.12, MLS.16, **LIGHT_BAYER 0x9300, MAIN_BAYER 0xB340, HIGH_BAYER 0xC340**,
  CHigh 444.12, TDC 444.12, TDC MLS 444.12; `xs_cpih_e` has NONE/RCT/**TETRIX**;
  `xs_tetrix_e` FULL/INLINE; `xs_cfa_pattern_t` RGGB/BGGR/GRBG/GBRG; NLT
  quadratic/extended with `xs_config_nlt_extended_auto_thresholds()`; TDC
  parameters (`xs_tpc_parameters_t`). Encoder config syntax (README): e.g.
  `jxs_encoder -c "profile=MainBayer;cfa=RGGB;cpih=tetrix,<Cf>,<e1>,<e2>;nlt=quadratic,<sigma>,<alpha>;rate=3" -w 3552 -h 3556 -d 12 in.mono out.jxs`
  (1-component input + `cfa=` for Bayer profiles), `-D`/`-DD` dumps the full
  configuration string of an existing codestream, `gains=psnr|visual` selects
  built-in gain/priority tables.
- The README warns it "represents just one way of implementing JPEG XS", gives
  no speed guarantee and "it is possible that bugs or mistakes exist"; the
  standard text is authoritative. No SIMD; expect it to be slow at 12.6 Mpixel.
- An unofficial GitHub copy exists (https://github.com/TangKii/jxs, 9 stars,
  "an integral part of ISO/IEC 21122-5 Reference Software", carries the same
  LICENSE.md); prefer the ISO zip as the source of truth.

### 1.5 CUDA / GPU JPEG XS

| Project | Kind | Status / notes | Source |
|---|---|---|---|
| Fastvideo (fastcompression.com) | Commercial CUDA encoder + decoder | 8-16 bit, 4:4:4/4:2:2/4:2:0/gray; decode 4K 4:4:4 24-bit @12.4:1: 301 fps (RTX 2070S), 425 (2080 Ti), 685 (RTX 4090); 2K: 880/1225/1740 fps. No Bayer-profile mention. Notes the JPEG XS PPL patent licence. | https://www.fastcompression.com/products/gpu-jpeg-xs.htm |
| intoPIX FastTicoXS | Commercial SDK, CPU + NVIDIA CUDA + OpenCL + ARM64/Jetson | Profiles HIGH, MAIN, MLS.12, TDC via "FIP"; 4:4:4/4:2:2/4:2:0/4:0:0, 8-16 bit. Bayer is sold separately as **TicoRAW** (proprietary, not JPEG XS). Tiers START/PRO/ULTIMATE. | https://www.intopix.com/fasttico-xs-sdks |
| Fraunhofer IIS JPEG XS SDK | Commercial SDK, x86, Apple ARM, Jetson (CPU), NVIDIA GPU (Windows) | "RGB/YCbCr (4:4:4, 4:2:2, 4:2:0) and CFA Bayer pattern"; 32 lines E2E latency; Holoscan for Media plugin with GPUDirect (2023-04). | https://www.iis.fraunhofer.de/en/ff/amm/content-production/jpegxs.html, https://www.iis.fraunhofer.de/en/pr/2024/20230412_AME_JPEG_Nvidia_holoscan.html |
| Liufangyu/jpeg-xs_cuda_decoder | Open GitHub, C/CUDA | Port of the ISO reference decoder to CUDA, single commit 2023-08-10, 1 star, "integration, testing, and modification have not yet been carried out", no licence file (and derived from the ISO code, so the ISO licence applies). Not usable as-is; possibly useful as a reading example. | https://github.com/Liufangyu/jpeg-xs_cuda_decoder |
| Bruns, Richter, Ahmed, Keinert, Foessel, "Decoding JPEG XS on a GPU", PCS 2018 | Paper | Register-based DWT on CUDA; content not read here (**unverified** beyond title/DOI). | https://doi.org/10.1109/PCS.2018.8456310 |
| NVIDIA nvJPEG / Holoscan | n/a | nvJPEG covers JPEG and JPEG 2000 only; Holoscan JPEG XS comes via Fraunhofer's plugin, not a native operator. | https://docs.nvidia.com/cuda/nvjpeg/index.html, TVBEurope https://www.tvbeurope.com/media-delivery/fraunhofer-iis-offers-jpeg-xs-plugin-for-nvidias-holoscan-for-media-architecture |

Net: **no open-source, licence-clean CUDA JPEG XS decoder exists**; writing one is
genuinely new work.

### 1.6 Open-source FPGA / RTL cores

None found. Searches for JPEG XS Verilog/VHDL on GitHub return only JPEG,
MJPEG and JPEG-LS cores (e.g. https://github.com/ultraembedded/core_jpeg,
https://github.com/lulinchen/jpeg_open). Two academic items surfaced but were
not read (**unverified**): "Implementation of JPEG XS entropy encoding and
decoding on FPGA" (preprint, https://doi.org/10.21203/rs.3.rs-3315591/v1) and
"An FPGA Implementation of Displacement Vector Search for Intra Pattern Copy in
JPEG XS" (https://arxiv.org/pdf/2603.10671). The IHSE/Fraunhofer core is written
in VHDL but is commercial (section 2.2).

---

## 2. Commercial FPGA IP cores

### 2.1 intoPIX TicoXS (JPEG XS) - the only vendor with a Lattice port

- Families (https://www.intopix.com/tico-xs-ip-cores): AMD Spartan-6/7, Artix-7,
  Kintex-7, UltraScale(+), Zynq, Versal; Intel Cyclone V/10, Arria V/10, Stratix
  V/10, Agilex; **Lattice CertusPro-NX and Avant**. Lattice's own IP page for
  "TICO-XS (JPEG XS) Encoder / Decoder IP-cores" lists CrossLink-NX, Certus-NX,
  CertusPro-NX, ECP2/M, ECP5/ECP5-5G and gives reference configurations:
  HD-60-422 (min 45 MHz), 4K-60-444 (min 240 MHz), 8K-60-422 (min 150 MHz); 8/10/12
  (16)-bit; latency "few lines of pixels only (1/10th of 1 millisecond)"
  (https://www.latticesemi.com/products/designsoftwareandip/intellectualproperty/ipcore/intopixcores/tico-xs-jpeg-xs-encoder-decoder).
- Press: "intoPIX Expands its Offering for Medical, Human & Machine Vision
  Applications with TicoRAW & JPEG-XS on Lattice Low-Power FPGAs" (2025-03-03,
  https://www.design-reuse.com/news/57481/intopix-lattice-fpga.html) and
  "... Automotive Innovation with TicoRAW & JPEG XS on Lattice ..." (2025-06-09,
  names Lattice Avant and Nexus; "up to 10X bandwidth reduction per camera",
  https://www.silicon.co.uk/press-release/intopix-accelerates-automotive-innovation-with-ticoraw-jpeg-xs-on-lattice-low-power-fpgas).
  Neither gives LUT/EBR numbers.
- Datasheet (Intel Marketplace PDF, "TicoXS Dec 2021",
  https://marketplace.intel.com/file-asset/a5Y3b0000016xJmEAI_a5b3b00000010XlAAI):
  "Compliancy JPEG XS standard (ISO/IEC 21122-1 - High/Main / MLS12 profiles)";
  4:4:4/4:2:2/4:2:0/4:0:0; 8-16 bit; "Latency selectable from 2 lines to 15 lines"
  for the IP cores (30 lines to 1 frame for the SDK); "Full transparency to
  uncompressed, down to 3bpp (according to ISO flicker test)"; "Visually lossless
  down to 1bpp"; CBR "adjustable down to 36:1 (1bpp)"; "No external DDR";
  reference cores IPX-TICO-XS-HD/UHD4K/UHD8K-60-444-12. TDC comes with the newer
  "TicoXS FIP" cores (NAB 2024 release, High 4:2:0 + TDC,
  https://www.design-reuse.com/news/15920-intopix-unveils-latest-jpeg-xs-fpga-cores-with-nextera-adeas-st2110-ipmx-streamlining-ipmx-development-at-nab-show/).
- **Resource footprints: not published** anywhere fetched ("extremely small",
  "up to 30% of resources" saved in the 2021 compact-core release,
  https://www.design-reuse.com/news/10760-intopix-releases-a-new-range-of-compact-encoders-and-decoders-for-jpeg-xs/).
  Numbers require an NDA/datasheet request (**unverified**).
- **Bayer**: intoPIX's raw-sensor product is **TicoRAW**, described as a
  proprietary "RAW (Bayer) image processing and compression technology" with its
  own SDK/IP (RGGB/RCCB/GBBR/RYYCy, 8-16 bit, "down to 0.1 millisecond", AMD,
  Intel, Lattice Certus Pro and Avant), with **no** claim of JPEG XS Bayer-profile
  conformance on its pages (https://www.intopix.com/tico-raw,
  https://www.intopix.com/tico-raw-fpga-asic-ip-cores). Whether intoPIX will
  license a *JPEG XS Bayer-profile* encoder core for Lattice is **unverified**
  (ask them; they co-authored the Bayer tools).
- Licensing model (broad): evaluation kits and reference designs
  (https://www.intopix.com/XS-SDK-and-FPGA-Development-Kit-for-Evaluation),
  per-project IP licences; royalty terms not public.

### 2.2 IHSE / Fraunhofer IIS JPEG XS FPGA IP core

Datasheet https://www.ihse.com/wp-content/uploads/files/data-sheets/jpeg-xs-handout.pdf
("Development in cooperation with Fraunhofer"; partnership PR
https://www.iis.fraunhofer.de/en/pr/2023/20230914_jpeg_xs_ihse.html):

| Device | Block | ALMs / LUTs | DSPs | Memory |
|---|---|---|---|---|
| Intel Cyclone 10 GX (High 444.12, level 4k-2, sublevel 12 bpp, 4K@60) | Encoder | 23,000 ALMs | 82 | 380 M20K |
| Intel Cyclone 10 GX | Decoder | 16,000 ALMs | 144 | 318 M20K |
| Xilinx UltraScale+ | Encoder | 42,000 LUTs | 96 | 104 BRAM + 19 URAM |

Features: "Written in VHDL", "Processor free", up to 10K, up to 480 fps, 4:4:4/
4:2:2, RGB/YCbCr, up to 16 bit, compression up to 16:1, and **"Future support of
Bayer pattern"** - i.e. not Bayer-capable at publication. No Lattice mention.
Licensing: contact jpegxs@ihse.com; no terms published.

Scale check against CertusPro-NX: the largest device, LFCPNX-100, has 96K logic
cells, 3,744 Kb EBR, 3,584 Kb LRAM and 156 18x18 multipliers; LFCPNX-50 has 52K
logic cells, 1,728 Kb EBR, 96 multipliers
(https://www.latticesemi.com/en/Products/FPGAandCPLD/CertusPro-NX). A 4K60
High-profile encoder at 42K UltraScale+ LUTs / 380 M20K (7.6 Mb) would not fit an
LFCPNX-50's EBR; a Bayer encoder at 3552x3556x33 fps (~0.42 Gpix/s) is roughly a
third of that pixel rate, and LightBayer (no vertical DWT) needs far less line
memory, so the budget is plausible but must be sized from the line buffers, not
from these numbers (interpretation, not a vendor claim).

### 2.3 Fraunhofer IIS (software SDK; FPGA via partners)

https://www.iis.fraunhofer.de/en/ff/amm/content-production/jpegxs.html: SDK for
x86 (Windows/Linux), Apple ARM, NVIDIA Jetson (CPU), NVIDIA GPU (Windows);
"RGB/YCbCr (4:4:4, 4:2:2, 4:2:0) and CFA Bayer pattern"; "Ultra low latency 32
lines end-to-end algorithmicwise"; compression 2:1 to 16:1; FPGA IP via IHSE and
Astrodesign. Brochure (https://www.iis.fraunhofer.de/content/dam/iis/de/doc/ame/bewegtbild/FraunhoferIIS_Product-Brochure_JPEG-XS.pdf):
"Low Latency (32 lines end-to-end)", "RGB/YCbCr 422, 444, 4224 and 4444 up to 12
Bit per component", "Predictive and precise ratecontrol", up to 10:1. SDK 5 is
"25 percent faster" (Fraunhofer PR via search summary, **unverified** page).

### 2.4 AMD/Xilinx and Intel/Altera

No first-party JPEG XS core from AMD or Intel was found; both are served by
intoPIX (section 2.1) and, for Intel, IHSE (section 2.2). AMD's broadcast page
points at partners (https://www.xilinx.com/applications/broadcast/encoder.html,
not fetched, **unverified**).

### 2.5 Lattice-specific summary

- Only intoPIX has announced JPEG XS on Lattice; CertusPro-NX is explicitly
  listed (Lattice IP page and intoPIX TicoXS page above). No public resource or
  power figures for the Lattice port.
- Lattice's own page states minimum clock per configuration (45 MHz HD60 4:2:2,
  240 MHz 4K60 4:4:4, 150 MHz 8K60 4:2:2 - the 8K figure implies a wider
  pixels-per-clock architecture), which is a useful sanity bound: CertusPro-NX
  fabric at ~150-240 MHz for a 4-pixel/clock datapath is aggressive.

### 2.6 Patent licensing (applies to home-grown implementations too)

JPEG XS standard-essential patents are pooled by Fraunhofer IIS and intoPIX and
administered by Vectis IP (https://www.jpegxspool.com/; PR
https://www.iis.fraunhofer.de/en/pr/2022/20220301_jpeg_xs.html). The published
"Overview of Terms" (v5.0, 2025-04-01,
https://halibut-recorder-z9me.squarespace.com/s/JPEG-XS-PPL-Overview-of-Terms-v50-01-Apr-2025.pdf)
defines three models: Pay Per Instance/Product (permanent products), Pay Per
Activation (time-limited), Pay Per Use (services). Model 1 Category 1 (first
40,000 USD of annual royalties): 3.00 USD per HD instance, 5.00 USD per 4K
instance, 9.00 USD per 8K instance, decreasing by category to 0.15 USD per
product at >15 M USD. Using SVT-JPEG-XS or writing your own codec does not
change this; only internal evaluation is outside the "Product/Service"
definitions (read the PDF; this is not legal advice).

---

## 3. Conformance and test material

### 3.1 ISO/IEC 21122-4 conformance package (public download)

- 3rd edition (ISO/IEC 21122-4:2025, stage 60.60 per
  https://jpeg.org/jpegxs/workplan.html) files at
  https://standards.iso.org/iso-iec/21122/-4/ed-3/en/ (ISO Customer Licence
  click-through; `curl` HEAD/GET succeeded without login):
  - `codestream-parser.zip` (54 KB): Python scripts by Thomas Richter
    (Fraunhofer) - `jxscodestream.py` (rev 1.22, 2024-05-14) dumps every marker
    and checks profile constraints for all 3rd-edition profiles incl. LightBayer/
    MainBayer/HighBayer, TDC, CHigh, MLS; `jp2file.py` handles the JXS file
    format. README: "They require a python 2.7 environment (python 3.x will
    *NOT* work on them)".
  - `difftest.zip` (440 KB) - reference-vs-decoded comparison tool
    (**unverified** contents, not downloaded).
  - `buffermodelchecker.zip` (10 KB) - Part 2 buffer-model checker
    (**unverified** contents, not downloaded).
  - `tests.zip` (1,853,424,387 bytes). Central directory listed remotely via HTTP
    range requests: 1,636 entries = 185 `.jxc` + 1 `.jxs` codestreams, 186 `.txt`
    parser dumps, 186 `.pgx` reference images and 535 per-component `.raw`/`.h`
    planes (largest planes 65.9 MB). Profile histogram from the 186 dumps:
    93 TDC 444.12 (frame sequences `301/`-`308/`), 10 Main 4444.12, 10 High
    4444.12, 9 Main 444.12, 8 MLS.12, 7 Light 422.10, 7 Light 444.12, 7 Main
    422.10, 7 High 444.12, 6 Light-Subline, 5 Unrestricted, 4 TDC MLS 444.12,
    **3 LightBayer (210-212), 3 MainBayer (213, 214, 216), 1 HighBayer (215)**,
    2 Main 420, 2 CHigh 444.12, 1 MLS.16 (`320`, Ppih 0x6ED0, unknown to that
    parser revision). The Bayer streams are 488x325 on the component grid, 4
    components, level 1k-1 @ 3 bpp, CAP bits {1,5,8} / {1,2,5,8} / {1,3,5,8} =
    Star-Tetrix + (quadratic | extended NLT) + component-dependent decomposition
    + raw-mode per packet. Streams 1-67 are 3-component 4:2:2/4:4:4 (and
    4-component / 1-component) 4064x2704, 4096x1744, 12287x1743 etc. at 3-12 bpp
    and match SVT's `DecoderConformanceTest.sh` list (section 1.1.11).
- 2nd edition package: https://standards.iso.org/iso-iec/21122/-4/ed-2/en/
  (`ISO_IEC 21122-4_ed2_en.zip`, 1,421 MB); the 2022 text says conformance
  codestreams, reference decoded images and auxiliary software are provided there
  (https://www.iso.org/standard/82568.html; sample PDF
  https://cdn.standards.iteh.ai/samples/82568/2c5f4f1917b54a0399a81e6cca695386/ISO-IEC-21122-4-2022.pdf).
- Licence for these files: ISO Customer Licence Agreement - "You are permitted
  to use the electronic insert(s) available on this site, in their original
  format without any modifications for the purposes specified in their respective
  ISO standard(s)" (index page). Fine for a private CI vector set; do not
  redistribute.

### 3.2 What in SVT-JPEG-XS is reusable as an oracle

- The **decoder** as a second-opinion decoder for any Main/High/Light stream and
  (untested here) for Bayer streams; bit-exact expectations only vs. the ISO
  `.pgx` references, since JPEG XS decoding is deterministic integer arithmetic.
- `tests/scripts/DecoderConformanceTest.sh` + `CommonLib.sh` as a ready harness
  pattern for "decode ISO stream NNN, `diff` against reference" (needs the ISO
  streams renamed to `test_bitsreams/NNN.jxs` and references converted to `.yuv`).
- The unit tests for DWT 5/3, quantisation, GCLI packing and Star-Tetrix inverse
  (`TestIDWT53.cc`, `TestDwtIdwt.cc`, `TestQuant.cc`, `TestPack.cc`,
  `TestInvDwtFrame.cc`) are useful *reading* for kernel-level golden tests.
- Not reusable: there are no shipped image vectors, no PSNR/quality oracles, and
  the encoder cannot generate Bayer-profile streams.

### 3.3 ISO reference software as oracle

`jxs_encoder`/`jxs_decoder` (section 1.4) encode and decode every profile; the
README recommends `jxs_decoder -D` to dump a conformance stream's configuration
and feed it back to the encoder, which is exactly how to generate reproducible
Bayer-profile test streams from IMX676 captures.

---

## 4. Practical numbers for raw / Bayer / high-bit-depth content

- JPEG committee white paper "JPEG XS in-depth series: Raw image compression"
  (WG1N100275, https://ds.jpeg.org/documents/jpegxs/wg1n100275-096-COM-JPEG_XS_in-depth_series_raw_image_compression.pdf):
  "compression factors between 6:1 and 12:1 with visually lossless quality and can
  do around 2.5:1 mathematically lossless compression" on raw Bayer; "compression
  at 3 bpp for 4K@60 content has proven to deliver visually lossless quality";
  the three Bayer profiles differ in vertical DWT levels (Light 0, Main 1, High
  2) and Star-Tetrix mode (Light: inline only; Main/High: full or inline); all
  support 10/12/14/16-bit samples and the optional quadratic/extended NLTs; the
  camera-to-CCU use case targets "ratios from 2:1 to 4:1 or better" at "<< 1ms".
  Figures 4-6 plot average PSNR at 1.0/1.5/2.0/3.0/4.0 bpp for 20 raw stills
  (12/14-bit): HighBayer beats JPEG-on-demosaiced-RGB "significantly" at higher
  rates and the three Bayer profiles are ordered Light < Main < High. The exact
  dB values are only in the bar charts and could not be read from the PDF text
  (**unverified**).
- Richter, Foessel, Descampe, Rouvroy, "Bayer CFA Pattern Compression With JPEG
  XS", IEEE TIP 30:6557-6569, 2021 (https://doi.org/10.1109/TIP.2021.3095421,
  abstract via Semantic Scholar): Star-Tetrix plus a pre-emphasis (NLT) gives "a
  gain over a RGB compression workflow in terms of complexity and quality
  (between 1.5dB and more than 4dB depending on the target bitrate)". Earlier:
  "Bayer Pattern Compression with JPEG XS", ICIP 2019
  (https://doi.org/10.1109/ICIP.2019.8803376) and "Rate Allocation for
  Bayer-Pattern Image Compression with JPEG XS", DCC 2019
  (https://doi.org/10.1109/DCC.2019.00040) - bodies not read (**unverified**).
- General visually-lossless range: "indistinguishable from the original ...
  (passing ISO/IEC 29170-2 tests, also called AIC-2) for compression ratios
  between 2:1 and 10:1"; end-to-end latency "between 1 and 32 lines"
  (https://en.wikipedia.org/wiki/JPEG_XS). intoPIX: transparent to 3 bpp,
  visually lossless to 1.5 bpp on natural content, CBR down to 1 bpp (36:1)
  (Intel Marketplace PDF, section 2.1). Fraunhofer/IHSE: 32 lines E2E, up to
  16:1 (sections 2.2/2.3). TicoRAW (proprietary, for calibration only): "At
  12-bit: 6:1 (2 bpp) to 12:1 (1 bpp)" (https://www.intopix.com/tico-raw).
- Applied to IMX676 (arithmetic, not a source claim): 3552 x 3556 = 12.63 Mpix;
  raw 12-bit = 151.6 Mbit/frame = 5.0 Gbit/s at 33 fps. At 4 bpp (3:1) ~1.67
  Gbit/s, at 3 bpp (4:1) ~1.25 Gbit/s, at 2 bpp (6:1) ~0.83 Gbit/s, at 1.5 bpp
  (8:1) ~0.63 Gbit/s. Bayer-profile bpp is counted per sensor sample, so the 3
  bpp "visually lossless" video figure above maps to 4:1 on 12-bit raw.
- Levels: Wikipedia's level table pairs each ordinary level with a "Bayer"
  level of double the name (1k-1/Bayer2k-1 max width 1,280; 2k-1/Bayer4k-1 max
  width 2,048, 4,194,304 max pixels, 133.7 Mpix/s; 4k-1/Bayer8k-1 4,096 wide,
  8,912,896 pixels, 267.4 Mpix/s), consistent with Star-Tetrix halving each
  dimension into 4 components. Read that way, IMX676 (component grid 1776 x
  1778 = 3.16 Mpix, 104 Mpix/s at 33 fps) fits **Bayer4k-1**; verify against
  ISO/IEC 21122-2:2024 Annex A before hard-coding Plev (**interpretation**).
- Latency in lines (encoder+decoder algorithmic): profile-dependent. LightBayer
  has no vertical DWT (precinct = 1 line of the component grid), MainBayer 1
  level, HighBayer 2 levels (white paper above); intoPIX IP "2 lines to 15
  lines" (section 2.1); Fraunhofer "32 lines" for its SDK (section 2.3);
  Wikipedia "1 to 32 lines". SVT's default `slice_height 16` with `decomp_v 2`
  is at the 32-line end.

---

## 5. Recommendation and pitfalls

### 5.1 Oracle strategy

1. **Primary oracle for Bayer-profile codestreams: ISO/IEC 21122-5 ed. 3
   `libjxs`** (section 1.4). It is the only open code that both encodes and
   decodes LightBayer/MainBayer/HighBayer with Star-Tetrix (full and inline),
   NLTs and `Sd`. Use it (a) to encode IMX676 captures into MainBayer/LightBayer
   streams with dumped `-DD` configs, (b) to decode FPGA output and compare
   sample-exact, (c) to cross-check the CUDA decoder. Keep it in a separate,
   clearly labelled third-party tree; its licence allows exactly this
   (evaluation and conformance testing) and nothing that ships.
2. **Fixed vectors: ISO/IEC 21122-4 ed. 3 `tests.zip`** (section 3.1), at
   minimum streams 210-216 (Bayer, all three profiles, both NLTs), 1-9 and
   28-35 (Main/High/Light 3-comp and 1-comp), 42-47 (4-component), 200-203
   (4:2:0). Store the `.jxc` + `.pgx` pairs privately; do not commit them (ISO
   licence). The 1.85 GB archive can be listed and partially fetched with HTTP
   range requests, as done here, to avoid downloading the TDC sequences.
3. **Secondary decoder oracle and fast path: SVT-JPEG-XS** (section 1.1). It
   decodes Cpih=3 + NLT streams and is the only permissively licensed,
   SIMD-optimised codec; use it for bit-exact decode comparisons of FPGA output
   in CI at speed, and as the encoder for any non-Bayer (RGB/YUV/4-plane)
   experiments. Confirm first that it decodes ISO streams 210-216 bit-exactly;
   if it does not, that is a bug report to Intel, not a reason to trust it less
   on Main/High.
4. **Header/marker oracle: `jxscodestream.py`** from `codestream-parser.zip`
   (Python 2.7 only; run it in a `python2` venv or port the ~1,800 lines) to
   diff CAP/PIH/CDT/WGT/CTS/CRG/NLT markers between FPGA output and reference
   output before comparing samples.
5. For the **CUDA decoder**, test at three levels: marker parse (against the
   Python parser), per-precinct coefficient dumps (patch `libjxs` to dump
   coefficients after entropy decoding and after dequantisation; its code is
   small and readable), and final samples (against `.pgx`/`libjxs` output).

### 5.2 Pitfalls

- **Bayer profiles are not available in any open encoder except the ISO
  reference software.** SVT-JPEG-XS cannot emit them (its own header comment,
  section 1.1.4); FFmpeg 8.1 and GStreamer 1.26 wrappers expose YCbCr only.
- **Edition mismatch.** Bayer tools are 2nd edition (2022); TDC, CHigh, MLS.16
  are 3rd edition (2024/2025) (https://en.wikipedia.org/wiki/JPEG_XS,
  https://jpeg.org/jpegxs/workplan.html, https://www.jpegxs.com/blogs_and_news/post/iso-iec-publishes-the-third-edition-of-the-jpeg-xs-standard.).
  A 1st-edition decoder rejects any CAP bit; SVT even has a `--cap-compat`
  mode that writes an *empty* CAP marker because "Some strict decoders reject a
  non-empty CAP marker" (`PackHeaders.c`). Bayer streams necessarily set CAP
  bits 1 (Star-Tetrix), 5 (Sd) and usually 2/3 (NLT), so every consumer of your
  FPGA stream must be at least 2nd-edition. Pin the edition in your codestream
  spec and test with the 2025 parser, whose revision 1.22 still lacks MLS.16.
- **Reference software licence and speed.** Evaluation/conformance-only
  licence, RAND for products, no patent grant; unoptimised C. Budget minutes,
  not milliseconds, per 12.6 Mpixel frame (**estimate**).
- **Patents.** Any shipped encoder/decoder instance (FPGA or CUDA) falls under
  the Vectis JPEG XS PPL (section 2.6); SVT's BSD+Patent grant does not cover
  intoPIX/Fraunhofer SEPs.
- **SVT specifics**: x86-64 only; 4:0:0 encode unsupported; 13/14-bit input is
  outside all lossy profiles; `slice_height` must be a multiple of
  2^`ndecomp_v`; `ndecomp_h >= ndecomp_v`; LSB vs MSB sample alignment is a
  configurable convention (`*_msb_aligned`) - match it to the CUDA decoder's
  output; only tag `v0.9.0` exists, so pin a commit; the ffmpeg-plugin's
  4-component mapping is out-of-tree.
- **Rate control is not conformance.** Any conformant codestream decodes
  identically everywhere; the encoder's precinct/slice budget strategy (SVT
  modes 0-3, `libjxs` `ra_budget_lines`, an FPGA's line-based CBR) only affects
  quality and buffer occupancy. Validate the FPGA encoder by decoding with two
  independent decoders (libjxs and SVT) and by running `buffermodelchecker`
  against the declared level/sublevel, not by expecting byte-identical streams
  to a software encoder.
- **Lattice fit is unquantified.** No vendor publishes CertusPro-NX numbers for
  JPEG XS; the only public footprints (IHSE, Cyclone 10 GX / UltraScale+ 4K60
  High 444.12) are for a non-Bayer core and exceed an LFCPNX-50's EBR if taken
  at face value. Size from line-buffer math for LightBayer/MainBayer at 0.42
  Gpix/s, and ask intoPIX whether a JPEG XS *Bayer-profile* (not TicoRAW) core
  exists for CertusPro-NX.

---

## Appendix A. Sources and verification status

| Source | Fetched? | Used for |
|---|---|---|
| https://github.com/OpenVisualCloud/SVT-JPEG-XS (+ shallow clone of `main` @ f9c82c1, 2026-09-18) | yes | 1.1 throughout |
| https://raw.githubusercontent.com/OpenVisualCloud/SVT-JPEG-XS/main/LICENSE.md | yes | licence |
| `Source/API/SvtJpegxs.h`, `SvtJpegxsEnc.h`, `SvtJpegxsDec.h` | yes | API |
| `Source/Lib/Encoder/Codec/ProfileLevel.h`, `PackHeaders.c`; `Source/Lib/Decoder/Codec/ParseHeader.c`, `Mct.c`, `NltDec.h` | yes (local grep) | profile/Bayer facts |
| https://github.com/OpenVisualCloud/SVT-JPEG-XS/tags, /releases, GitHub API | yes | versions |
| https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/Changelog; https://ffmpeg.org/index.html; https://ffmpeg.org/doxygen/trunk/libsvtjpegxsenc_8c_source.html; https://www.phoronix.com/news/FFmpeg-Merges-JPEG-XS | yes | 1.2 |
| https://gstreamer.freedesktop.org/releases/1.26/; .../documentation/svtjpegxs/svtjpegxsenc.html, svtjpegxsdec.html | yes | 1.3 |
| https://standards.iso.org/iso-iec/21122/-5/ed-3/en/ (zip downloaded, LICENSE/README/libjxs.h read) | yes | 1.4 |
| https://jpeg.org/jpegxs/software.html; https://jpeg.org/jpegxs/workplan.html; https://jpeg.org/jpegxs/ | yes | 1.4, 3, 4 |
| https://github.com/TangKii/jxs | yes | 1.4 |
| https://standards.iso.org/iso-iec/21122/-4/ed-3/en/ (index; codestream-parser.zip downloaded; tests.zip central directory + all 186 .txt read via range requests) | yes | 3.1 |
| https://standards.iso.org/iso-iec/21122/-4/ed-2/en/ | yes (index only) | 3.1 |
| https://ds.jpeg.org/documents/jpegxs/wg1n100275-096-COM-JPEG_XS_in-depth_series_raw_image_compression.pdf | yes (via curl -k; TLS chain incomplete) | 4 |
| https://doi.org/10.1109/TIP.2021.3095421 (abstract via api.semanticscholar.org); ICIP/DCC 2019 and PCS 2018 DOIs via api.crossref.org | abstract/metadata only | 4, 1.5 |
| https://en.wikipedia.org/wiki/JPEG_XS | yes | profiles/levels/editions |
| https://www.intopix.com/tico-xs-ip-cores, /fasttico-xs-sdks, /tico-raw, /tico-raw-fpga-asic-ip-cores; Intel Marketplace PDF a5Y3b0000016xJmEAI | yes | 2.1, 1.5 |
| https://www.latticesemi.com/products/designsoftwareandip/intellectualproperty/ipcore/intopixcores/tico-xs-jpeg-xs-encoder-decoder; https://www.latticesemi.com/en/Products/FPGAandCPLD/CertusPro-NX | yes | 2.1, 2.2, 2.5 |
| https://www.design-reuse.com/news/57481/intopix-lattice-fpga.html; https://www.silicon.co.uk/press-release/intopix-accelerates-automotive-innovation-with-ticoraw-jpeg-xs-on-lattice-low-power-fpgas; design-reuse 15920, 10760 | yes | 2.1 |
| https://www.ihse.com/wp-content/uploads/files/data-sheets/jpeg-xs-handout.pdf (pdftotext) | yes | 2.2 |
| https://www.iis.fraunhofer.de/en/ff/amm/content-production/jpegxs.html; .../pr/2024/20230412_AME_JPEG_Nvidia_holoscan.html; .../pr/2023/20230914_jpeg_xs_ihse.html; brochure PDF | yes | 2.3, 1.5 |
| https://www.fastcompression.com/products/gpu-jpeg-xs.htm | yes | 1.5 |
| https://github.com/Liufangyu/jpeg-xs_cuda_decoder | yes | 1.5 |
| https://www.jpegxspool.com/; PPL Overview of Terms v5.0 PDF; https://www.iis.fraunhofer.de/en/pr/2022/20220301_jpeg_xs.html | yes | 2.6 |
| https://registry.bazel.build/modules/rules_foreign_cc; https://raw.githubusercontent.com/bazel-contrib/rules_foreign_cc/main/docs/README.md | yes | 1.1.10 |
| https://openbenchmarking.org/test/pts/svt-jpeg-xs | **no (HTTP 403)** | 1.1.9 unverified |
| https://www.businesswire.com/... (intoPIX Lattice PRs) | **no (HTTP 403)**; mirrored text used | 2.1 |
| https://dl.acm.org/doi/10.1109/TIP.2021.3095421, https://publica.fraunhofer.de/... | **no (anti-bot page)** | replaced by Semantic Scholar/Crossref |
| arXiv 2603.10671; Research Square rs-3315591 | **not read** | 1.6 unverified |
