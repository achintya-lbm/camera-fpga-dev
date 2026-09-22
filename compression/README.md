# compression — JPEG XS (ISO/IEC 21122)

Cuts the per-camera link rate (4.1–4.9 Gbit/s per IMX676 at its DA322 ceiling) by 3–4× so that four
cameras fit the 10G link. Design and parameter set: `DESIGN.md` §17; milestones: `TODO.md` M7.

| Path | Content |
|---|---|
| `docs/jpegxs_part1_notes.md` | Implementation notes on ISO/IEC 21122-1:2024 (clause-cited): codestream syntax, decoding process, band/precinct geometry, Star-Tetrix for Bayer, latency/parallelism, errata, worked IMX676 parameters |
| `docs/jpegxs_landscape.md` | Software/FPGA implementations, oracle choice (ISO 21122-5 `libjxs` + 21122-4 vectors + SVT-JPEG-XS), commercial IP, patents, numbers for raw Bayer |
| `jxs/` | Bit-exact C++20 reference codec (golden model for the CUDA decoder and the RTL): `codestream` (Annex A/B/C headers + geometry), `entropy` (Annex C/D, decoder + encoder), `transform` (Annex E/F/G), `decoder` + `jxs_decode`, `jxs_info`. Decoder verified sample-exact against the ISO decoder and the 21122-4 Bayer vectors (`tools/verify_decoder.sh`) — M7.1 |
| `cuda/`, `ops/` | `JpegXsDecodeOp`: CUDA decoder as a Holoscan operator replacing `CsiToBayerOp` — M7.3 |
| `tools/` | `raw_to_pgm.py` (CSI RAW10/12 dump → 16-bit PGM mosaic), `compare_pgm.py` (PSNR / bit-exactness, reads ISO `.pgx` too), `fetch_conformance.py` (ISO 21122-4 vectors by HTTP range requests into `testdata/`, never committed), `verify_decoder.sh` (decoder acceptance) |

Key choices (see §17.2): Bayer coded as four components on the 1776×1778 super-pixel grid with the
Star-Tetrix transform (`Cpih = 3`), `NL,x = 5`, `NL,y = 2` (8-sensor-row precincts), `Sd = 1`, CBR rate
control so the codestream rides the existing HSB frame transport (`bytes_written` = codestream length).
