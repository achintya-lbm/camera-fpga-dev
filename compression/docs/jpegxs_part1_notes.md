# JPEG XS Part 1 (ISO/IEC 21122-1:2024) — implementation notes

Source: ISO/IEC 21122-1:2024(en), *Information technology — JPEG XS low-latency lightweight image
coding system — Part 1: Core coding system*, third edition, 2024-07, 114 printed pages.
Local copy: `~/Downloads/ISO_IEC_21122-1_2024(en).pdf` (120 PDF pages; **printed page N = PDF page N+4**).

These notes are written for the DA322 team (FPGA encoder on the CertusPro-NX, CUDA decoder on the
Holoscan host, software encoder for validation). Every statement carries a clause/table reference so it
can be checked against the standard. Page numbers are printed page numbers. Where these notes go beyond
the standard (worked examples for our sensor, observations) this is marked **[derived]**.

Notation follows the standard (Clause 3.3, 4.2): `>>`/`<<` are arithmetic shifts (`x>>s = floor(x/2^s)`),
`umod` is the non-negative modulo, `⌈⌉`/`⌊⌋` are ceil/floor, `u(n)` is an n-bit unsigned big-endian
field, `pad(n)` pads to an n-bit boundary, `fill()` is an arbitrary run of filler bytes whose count is
inferred from a length field (A.1.2).

---

## 0. Symbol cheat sheet (Clause 3.3, p. 6–9)

| Symbol | Meaning |
|---|---|
| `Wf, Hf` | width/height of the *sampling grid* (PIH). For CFA data one grid point = one 2x2 super pixel (5.2) |
| `Nc` | number of components (1..8) |
| `B[i]`, `sx[i]`, `sy[i]` | bit depth (8..16) and horizontal/vertical sampling factor (1 or 2) of component i (CDT) |
| `Wc[i], Hc[i]` | component dimensions `⌈Wf/sx⌉`, `⌈Hf/sy⌉` (B.1) |
| `NL,x`, `NL,y` | max horizontal / vertical DWT levels (PIH) |
| `N'L,x[i]`, `N'L,y[i]` | per-component levels (B.2) |
| `Sd` | number of components (the *last* Sd) excluded from the DWT (CWD marker) |
| `β`, `Nβ` | wavelet filter type index and count (B.3) |
| `b`, `NL` | band index (component + filter type) and band count (B.3) |
| `bx[β,i]`, `b'x[b]` | band existence flags (B.4) |
| `dx[β,i]`, `dy[β,i]` | horizontal/vertical decomposition depth of filter type β in component i |
| `Wb[β,i], Hb[β,i]` | band dimensions in coefficients (B.2) |
| `Cw`, `Cs` | precinct-column width in units of `8·max(sx)·2^NL,x` grid columns (PIH) / in grid columns (B.5) |
| `Np,x`, `Np,y` | precincts per row / per column (B.5) |
| `Hp = 2^NL,y` | precinct height in grid lines (B.5) |
| `Hsl` | slice height in precincts (PIH) |
| `p`, `λ`, `s` | precinct index (raster), line within precinct, packet index |
| `L0[p,b]`, `L1[p,b]` | first / last+1 line of band b in precinct p (B.6) |
| `I[p,λ,b,s]` | line inclusion flag: line λ of band b is in packet s (B.7) |
| `Wpb[p,b]` | width of band b in precinct p (coefficients) (B.5) |
| `Ng=4`, `Ss=8`, `Si=8` | coefficients per code group, code groups per significance group, per TDC group |
| `Ncg`, `Ns`, `Ni` | number of code groups / significance groups / TDC groups per line (B.8–B.10) |
| `M[p,λ,b,g]` | bitplane count of code group g |
| `T[p,b]` | truncation position (LSB bitplanes dropped) of band b in precinct p (C.6.2) |
| `Q[p]`, `R[p]` | precinct quantization and refinement (precinct header) |
| `G[b]`, `P[b]` | band gain and priority (WGT); `Gr, Pr` refresh variants (WGR) |
| `D[p,b]` | bitplane-count coding mode, 2 bits (C.2) |
| `Dr[p,s]` | raw-mode override per packet (C.3) |
| `Z[p,λ,b,j]` | significance flag of group j (**1 = insignificant**, see 3.3) |
| `v[..]`, `s[..]` | quantization index magnitude and sign |
| `c[..]`, `c'[..]` | wavelet coefficient residual (after dequant) / wavelet coefficient (after TDC; = c in intra) |
| `Fq`, `Bw`, `Br` | fractional bits, nominal coefficient precision, bits per raw bitplane count |
| `O[c,x,y]`, `Ω[c,x,y]`, `R[c,x,y]` | IDWT output, inverse-MCT output, final samples |
| `Cpih` | colour transform: 0 none, 1 RCT, 3 Star-Tetrix |
| `Cf`, `e1`, `e2` | Star-Tetrix extent flag and chroma exponents (CTS) |
| `Ct`, `δx[c], δy[c]`, `k[δx,δy]` | CFA pattern type, sub-pixel displacement of coded component c, inverse map (F.5.6) |
| `Isl`, `Ysl` | slice TDC flag (0 SLH, 1 SLI) and slice index |
| `f[..]`, `Y[..]`, `Di[p,b]` | frame buffer, TDC selection flags, TDC mode (Annex H) |

---

## 1. Document scope and structure

### 1.1 What Part 1 defines
* Clause 1 (p. 1): Part 1 specifies (a) the **decoding process** from codestream to reconstructed
  image, (b) the **codestream syntax**, and (c) *guidance* (informative) on encoding. Target:
  continuous-tone grey-scale or colour images "without visual loss at moderate compression rates",
  typically 2:1 to 18:1; latency limited to a fraction of a frame. **Channel buffer models are out of
  scope.**
* Clause 2: no normative references.
* Clause 6 (p. 13): an encoder is anything that produces a conforming codestream. Annex C–G contain
  informative encoder subclauses. Clause 7.2 (p. 15): only externally observable output is normative,
  "up to a conformance-level dependent error bound specified in ISO/IEC 21122-4"; internal ordering of
  operations is free.

### 1.2 What is delegated to other parts (as stated in Part 1)
| Topic | Where | Part 1 reference |
|---|---|---|
| Profiles, levels, sublevels, buffer models | ISO/IEC 21122-2 | A.2 NOTE 2 (p. 17); PIH fields `Ppih`, `Plev` (Table A.7) "see ISO/IEC 21122-2"; SLH/SLI "depending on profile" (Table A.2); H.9 (`Qf`,`Rf` normative requirements in Part 2) |
| Decoder conformance and test procedures, error bounds | ISO/IEC 21122-4 | 7.2 (p. 15) |
| Transport / resynchronisation | "lower-level transport mechanisms", not named | A.3 NOTE 2 (p. 19), C.1 NOTE 1 (p. 45): the codestream has **no byte-stuffing**; markers can appear inside entropy-coded data, so a decoder cannot resynchronise by scanning for markers |
| Reference software (Part 5), transport (Part 3) | not referenced in the body | — |

Bibliography (p. 114) lists only ISO/IEC 10646 (UCS, for COM strings).

### 1.3 Edition and changes (Foreword, p. iv)
Third edition 2024-07 cancels and replaces the second edition (2022). Main changes:
1. "coding tools for improving the compression rates for screen content images" — in the body this
   appears as the **run mode `Rm`**, the **packet-based raw-mode switch `Rl`** (CAP bit 8) and the
   **`Br = 5`** option;
2. "coding tools that enable lossless coding of images with up to 16 bits per sample" — the
   `Bw = B[0], Fq = 0` mode (Table A.8, CAP bit 6).

Also new relative to 2022 (3.1.25 NOTE: "ISO/IEC 21122-1:2022 only defined intra coding tools"):
**temporal differential coding (TDC)** with the SLI slice header, TPC marker, WGR table, precinct
`Qf/Rf/Di` fields, the TDC subpacket and the wavelet-domain frame buffer of Annex H (CAP bit 9).

### 1.4 Document map
| Clause / Annex | Content | Normative? |
|---|---|---|
| 3 | Terms, symbols | — |
| 5 | Functional concepts: sample grid, CFA interpretation (5.2), wavelet/precinct/slice overview, codestream | — |
| 7 | Decoder overview (Figure 2, p. 14) | — |
| A | Codestream syntax, all markers | yes |
| B | Image data structures: bands, precincts, lines, packets, code groups, slices | yes |
| C | Entropy decoding: precinct/packet headers, subpackets, bitplane-count decoding, VLC | yes |
| D | Quantization (D.2/D.3 inverse normative; D.4–D.6 encoder guidance) | yes |
| E | DWT (E.2–E.8 inverse normative; E.9–E.14 forward guidance) | yes |
| F | Multiple component transforms: RCT (F.3), Star-Tetrix (F.5), forward versions (F.4, F.6) | yes |
| G | DC shift, non-linear transform, clipping (G.2–G.5 decoder; G.6–G.10 encoder) | yes |
| H | Frame buffer / TDC | yes |
| I | Example weight tables | informative |

---

## 2. Codestream syntax (Annex A)

### 2.1 General rules
* Codestream = ordered sequence of *markers*, *marker segments* and *entropy-coded data* (A.1.1).
  Bit order: bit strings and VLCs appear "left bit first", numeric values MSB first (A.1.2).
* Every marker is `0xFF` followed by a byte that is neither `0x00` nor `0xFF` (A.3). A marker segment is
  marker (2 bytes) + length field `u(16)` (counts payload **plus the 2-byte length field itself, not
  the marker**) + payload, big-endian (A.3).
* A wrong length field makes the codestream ill-formed; decoders "should check" (A.3). Optional
  segments may be skipped by length. Unknown required capability => abort (A.3).
* **No bit- or byte-stuffing**: marker byte patterns can occur inside entropy-coded data (A.3 NOTE 2).
* `Lcod` (PIH) gives total size SOC..EOC for CBR streams, 0 for VBR (Table A.7).
* Overall order (Table A.1, p. 17–18; only marker placement rules in A.4 are normative):

```
SOC  CAP  PIH  CDT  WGT  [WGR]  [NLT]  [CWD]  [CTS]  [CRG]  [TPC]  [COM]*
for each slice t (top to bottom):
    SLH | SLI                          # slice header (one or the other)
    for each precinct p in slice:
        compute_packet_inclusion(p)    # Table B.4 (decoder-side computation, no bits)
        precinct_header(p)             # Table C.1
        for s in 0..Npc[p]-1:
            packet_header(p,s)         # Table C.4
            packet_body(p,s)           # Table C.5
        fill()                         # filler bytes to reach Lprc[p]
EOC
```
All header markers (CDT, WGT, WGR, NLT, CWD, CTS, CRG, TPC, COM) must follow PIH and precede the first
slice header (A.4.5–A.4.13). SOC first, CAP second, PIH third (A.4.1, A.4.3, A.4.4).

### 2.2 Marker list (Table A.2, p. 19)
| Code | Symbol | Name | Presence | Clause |
|---|---|---|---|---|
| 0xFF10 | SOC | Start of codestream | mandatory, first | A.4.1 |
| 0xFF50 | CAP | Capabilities | mandatory, second | A.4.3 |
| 0xFF12 | PIH | Picture header | mandatory, third, exactly one | A.4.4 |
| 0xFF13 | CDT | Component table | mandatory, exactly one | A.4.5 |
| 0xFF14 | WGT | Weights table | mandatory, exactly one | A.4.12 |
| 0xFF1B | WGR | Refresh weights table | optional (0 or 1) | A.4.13 |
| 0xFF16 | NLT | Nonlinearity | optional (0 or 1); forbidden if `Fq = 0` | A.4.6 |
| 0xFF17 | CWD | Component-dependent wavelet decomposition | optional (0 or 1), only if `Nc > 3` | A.4.7 |
| 0xFF18 | CTS | Colour transformation spec (Star-Tetrix) | **mandatory iff `Cpih = 3`**, else forbidden | A.4.8 |
| 0xFF19 | CRG | Component registration | optional; **mandatory if `Cpih = 3`** | A.4.9 |
| 0xFF1A | TPC | Temporal prediction control | optional (0 or 1) | A.4.10 |
| 0xFF15 | COM | Extension | optional (0 or more) | A.4.11 (Table A.2 misprints "A.4.10") |
| 0xFF20 | SLH | Slice header (intra) | one per slice, profile-dependent | A.4.14 |
| 0xFF21 | SLI | TDC-enabling slice header | one per slice, profile-dependent | A.4.15 |
| 0xFF11 | EOC | End of codestream | mandatory, last, exactly one | A.4.2 |
| other | — | reserved for ISO/IEC | — | — |

Precinct headers and packet headers are **not** markers (C.1: "Precincts are not enclosed in markers").

### 2.3 SOC, EOC (Tables A.3, A.4, p. 20)
Bare 2-byte markers, no length field. `SOC = 0xFF10`, `EOC = 0xFF11`.

### 2.4 CAP — capabilities (A.4.3, Tables A.5–A.6, p. 20–21)
```
CAP    u(16) = 0xFF50
Lcap   u(16)            # segment length; bits = (Lcap-2)*8
cap[i] u(1)  for i in 0..(Lcap-2)*8-1   # 1 = capability i required to decode
pad(8)
```
`Lcap` must be chosen so that the last payload byte is non-zero when `Lcap > 2`; an empty array
(`Lcap = 2`) is allowed. Bit 0 is intentionally unused.

| Bit | Value 1 means the decoder needs |
|---|---|
| 1 | Star-Tetrix transform and CTS marker |
| 2 | Quadratic non-linear transform (NLT `Tnlt=1`) |
| 3 | Extended non-linear transform (NLT `Tnlt=2`) |
| 4 | A component with `sy[i] > 1` is present (vertical subsampling) |
| 5 | Component-dependent wavelet decomposition (CWD, `Sd > 0`) |
| 6 | Lossless decoding (`Bw = B[0], Fq = 0`) |
| 8 | Packet-based raw-mode switch (`Rl = 1`) |
| 9 | TDC (frame buffer required) |
| others | reserved, shall be 0 |

### 2.5 PIH — picture header (A.4.4, Table A.7, p. 22–23) — complete field list
Fixed size: `Lpih = 26` (208 payload bits).

| Field | Size | Meaning | Allowed values |
|---|---|---|---|
| `PIH` | u(16) | marker | 0xFF12 |
| `Lpih` | u(16) | segment length | 26 |
| `Lcod` | u(32) | total codestream size SOC..EOC incl. all markers (CBR); 0 for VBR | 0..2^32-1 |
| `Ppih` | u(16) | profile (Part 2); decoders should abort on unknown profile | 0 = no restrictions |
| `Plev` | u(16) | level + sublevel (Part 2) | 0 = no restrictions |
| `Wf` | u(16) | sampling-grid width | `max_i(sx[i])·2^NL,x` .. 2^16-1 |
| `Hf` | u(16) | sampling-grid height | `max_i(sy[i])·2^NL,y` .. 2^16-1 |
| `Cw` | u(16) | precinct-column width in multiples of `8·max_i(sx[i])·2^NL,x` grid columns (all but rightmost column); 0 = precincts span the full width | `Cw = 0` **or** `Wf umod (8·Cw·max_i(sx)·2^NL,x) ≥ max_i(sx)·2^NL,x` |
| `Hsl` | u(16) | slice height in precincts (all but last slice) | 1..2^16-1 |
| `Nc` | u(8) | number of components | 1..8 |
| `Ng` | u(8) | coefficients per code group | **4** |
| `Ss` | u(8) | code groups per significance group | **8** |
| `Bw` | u(8) | nominal wavelet-coefficient precision | 20, 18 or `B[0]` (Table A.8) |
| `Fq` | u(4) | fractional bits of wavelet coefficients | 8, 6 or 0 (Table A.8) |
| `Br` | u(4) | bits per raw-coded bitplane count | 4 or 5 (Table A.8) |
| `Fslc` | u(1) | slice coding mode | 0 = DWT runs across slice boundaries; 1 reserved (Table A.14) |
| `Ppoc` | u(3) | progression order | 0 = B.7 order (resolution-line-band-component); 1–7 reserved (Table A.13) |
| `Cpih` | u(4) | colour transform | 0 none, 1 RCT, 3 Star-Tetrix; 2, 4–15 reserved (Table A.9) |
| `NL,x` | u(4) | horizontal DWT levels | 1..8 |
| `NL,y` | u(4) | vertical DWT levels (of non-vertically-subsampled components) | `max_i(log2 sy[i])` .. `min(NL,x, 6)` |
| `Lh` | u(1) | long packet-header enforcement | 0/1 |
| `Rl` | u(1) | raw-mode selection per packet (else per band) | 0/1 |
| `Qpih` | u(2) | quantizer | 0 deadzone, 1 uniform; 2–3 reserved (Table A.10) |
| `Fs` | u(2) | sign handling | 0 signs inside data subpacket, 1 separate sign subpacket; 2–3 reserved (Table A.11) |
| `Rm` | u(2) | run mode | 0 runs = zero prediction residuals, 1 runs = zero coefficients; 2–3 reserved (Table A.12) |

Table A.8 — valid `(Bw, Fq, Br)` combinations:

| `Bw` | `Fq` | `Br` | Extra constraints | Use |
|---|---|---|---|---|
| `B[0]` | 0 | 4 | all `B[i] = B[0]`, `B[0] ≤ 12` | lossless, ≤12-bit sources |
| `B[0]` | 0 | 5 | all `B[i] = B[0]`, `B[0] > 12` | lossless, >12-bit sources |
| 18 | 6 | 4 | NLT present; CAP bit 2 or 3 set | non-linearity in use |
| 20 | 8 | 4 | CAP bits 2, 3 absent or 0 | **regular lossy case** |

NOTE 1 (p. 23): the `Cw` condition guarantees ≥ 8 LL samples per line in all non-rightmost precincts
and that no band of the rightmost precinct is empty. NOTE 2: when `Wf·Nc > 16376` and `NL,y = 0`, or
components skip the DWT, subpacket sizes can exceed the short-header ranges (`Lsgn ≥ 2048`,
`Ldat ≥ 32768`); use `Lh = 1`. `Lh = 0` is safe whenever ≥ 1 vertical decomposition is performed.

### 2.6 CDT — component table (A.4.5, Table A.15, p. 24)
```
CDT   u(16) = 0xFF13
Lcdt  u(16) = 2*Nc + 2
for c in 0..Nc-1:
    B[c]   u(8)   # bit depth 8..16
    sx[c]  u(4)   # 1 or 2 for components 1 and 2; must be 1 for all others
    sy[c]  u(4)   # 1 .. sx[c]
```
Thus only components 1 and 2 may be subsampled (4:2:2 `sx=2,sy=1`; 4:2:0 `sx=sy=2`). For CFA/Star-Tetrix
all four components must have `sx = sy = 1` (Table A.15 + F.2).

### 2.7 WGT — weights table (A.4.12, Table A.25, p. 29) and WGR — refresh weights (A.4.13, Table A.26, p. 30)
```
WGT   u(16) = 0xFF14          |   WGR   u(16) = 0xFF1B
Lwgt  u(16)  (variable)       |   Lwgr  u(16)
for b in 0..NL-1:             |   for b in 0..NL-1:
    if b'x[b]:                |       if b'x[b]:
        G[b]  u(8)  0..15     |           Gr[b] u(8) 0..15
        P[b]  u(8)  0..255    |           Pr[b] u(8) 0..255
```
Only *existing* bands (B.4) have entries, so the decoder must compute `b'x[]` from PIH/CDT/CWD before
parsing WGT. `G[b]` = gain (bitplanes protected from truncation), `P[b]` = priority for refinement
(C.6.2). If WGR is absent, `Gr = G`, `Pr = P`. WGR is used only for bands coded with `Di[p,b] = 1`
(forced refresh in TDC slices; Table C.3). Annex I gives PSNR-optimised examples (Appendix A below).

### 2.8 NLT — nonlinearity (A.4.6, Tables A.16–A.17, p. 25)
```
NLT   u(16) = 0xFF16
Lnlt  u(16) = 5 (Tnlt=1) or 12 (Tnlt=2)
Tnlt  u(8)   # 1 quadratic, 2 extended, others reserved
if Tnlt == 1:  σ u(1); α u(15);  DCO = α − σ·2^15           # signed DC offset
if Tnlt == 2:  T1 u(32) 1..2^Bw−1;  T2 u(32) 1..2^Bw−1;  E u(8) 1..4
```
Forbidden when `Fq = 0` (lossless). Selects output scaling per Table G.1 (see 3.11).

### 2.9 CWD — component-dependent wavelet decomposition (A.4.7, Table A.18, p. 26)
```
CWD   u(16) = 0xFF17
Lcwd  u(16) = 3
Sd    u(8)   1..Nc−1     # the LAST Sd components are NOT wavelet-decomposed
```
Only allowed if `Nc > 3`. Excluded components must have `sx = sy = 1`. Absent => `Sd = 0`. Annex I's
CFA examples use `Sd = 1` (component 3 = Δ of Star-Tetrix is sent without DWT).

### 2.10 CTS — colour transformation specification (A.4.8, Tables A.19–A.20, p. 26)
```
CTS       u(16) = 0xFF18
Lcts      u(16) = 4
Reserved  u(4) = 0
Cf        u(4)   # 0 = full transform (needs lines above and below); 3 = restricted in-line (no neighbouring lines); others reserved
e1        u(4)   0..3   # exponent of first chroma component (weight 2^e1)
e2        u(4)   0..3   # exponent of second chroma component
```
Present iff `Cpih = 3`.

### 2.11 CRG — component registration (A.4.9, Table A.21, p. 27)
```
CRG    u(16) = 0xFF19
Lcrg   u(16) = 2 + 4*Nc
for c in 0..Nc-1:
    Xcrg[c] u(16)   # horizontal offset of component c from grid point, units of 1/65536 grid spacing; 32768 = half way to next grid point
    Ycrg[c] u(16)   # vertical offset, same units
```
Absent => all components at grid vertices. Mandatory (exactly one) when `Cpih = 3`; it defines the CFA
pattern type `Ct` through Table F.9 (see 5.3).

### 2.12 TPC — temporal prediction control (A.4.10, Table A.22, p. 27–28)
```
TPC    u(16) = 0xFF1A
Ltpc   u(16) = 4 + 2*NL
Si     u(8)  = 8                     # TDC selection group size (code groups)
σbi u(1); αbi u(3);  Qbi = αbi − σbi·2^3     # quant adjustment for intra-coded-by-rate coefficients (−8..7)
σbr u(1); αbr u(3);  Qbr = αbr − σbr·2^3     # quant adjustment for refresh-coded coefficients
for b in 0..NL-1:
    Yh[b]  u(8)  0..255              # positional hash value for intra refresh
    Sh[b]  u(8)  1..7                # hash mask exponent
```
Absent => `Qbi = Qbr = 0`, `Si = 8`, `Sh[b] = −∞` (no position ever matches). Note the loop is over
all `NL` bands, not only existing ones.

### 2.13 COM — extension (A.4.11, Tables A.23–A.24, p. 28–29)
```
COM   u(16) = 0xFF15
Lcom  u(16) ≥ 4
Tcom  u(16)   # 0x0000 vendor string (zero-terminated ISO 10646); 0x0001 copyright string; 0x8000–0xFFFF vendor-specific; else reserved
Dcom  variable
pad(8)
```

### 2.14 SLH / SLI — slice headers (A.4.14–A.4.15, Tables A.27–A.28, p. 30–31)
```
SLH  u(16) = 0xFF20 | SLI u(16) = 0xFF21
Lslh u(16) = 4     | Lsli u(16) = 4
Ysl  u(16)         # slice index counting from 0 at the top
                   # SLH: Isl = 0 (intra only: c' = c).  SLI: Isl = 1 (precincts may use the frame buffer)
```
Slices appear in increasing order top to bottom; the slice's entropy-coded data runs to the next slice
header or EOC. Whether SLI is allowed depends on the profile (Part 2).

### 2.15 Precinct header (C.2, Table C.1, p. 46–47) — not a marker
```
Lprc[p]  u(24)  1..2^20−1   # bytes from the END of this precinct header to the byte before the next precinct header / slice header / EOC (packets + filler)
Q[p]     u(8)   0..31       # precinct quantization
R[p]     u(8)   0..2·NL−1   # precinct refinement
if Isl > 0:
    Qf[p] u(8) 0..31        # frame-buffer quantization
    Rf[p] u(8) 0..2·NL−1    # frame-buffer refinement
else: Qf[p] = 32; Rf[p] = 0
for b in 0..NL-1: if b'x[b]: D[p,b]  u(2)     # bitplane-count coding mode (Table C.2)
if Isl > 0:
    for b in 0..NL-1: if b'x[b]: Di[p,b] u(2) # TDC mode (Table C.3)
pad(8)
```
Table C.2 — `D[p,b]`: bit 0: 0 = predict from zero (no prediction), 1 = vertical prediction;
bit 1: 0 = significance coding disabled, 1 = enabled. The same number of `D` fields is present even
when bands are cut off at the bottom of the image (C.2). Vertical prediction is **forbidden for the
top precinct row of a slice** (C.2, C.6.1, C.6.3) — this is what makes slices independently decodable.

Table C.3 — `Di[p,b]` (binary): `00` intra, regular weights; `01` intra, **refresh** weights (WGR);
`10` temporal prediction except hash-matched positions; `11` TDC per selection group via `Y` flags
(hash-matched positions still intra).

### 2.16 Packet header (C.3, Table C.4, p. 49–50)
Present only if the packet contains at least one included line (`ph = 1`).
```
Dr[p,s]  u(1)   # raw-mode override: 1 => bitplane counts of this packet in raw mode, no significance subpacket
if (Wf·Nc < 32752 && Lh == 0):     # SHORT header, 5 bytes total
    Ldat[p,s] u(15)  0..32767      # data subpacket bytes
    Lcnt[p,s] u(13)  0..8191       # bitplane-count subpacket bytes
    Lsgn[p,s] u(11)  0..2047       # sign subpacket bytes (present but ignored if Fs = 0)
else:                              # LONG header, 7 bytes total
    Ldat[p,s] u(20); Lcnt[p,s] u(20); Lsgn[p,s] u(15)
```
The size of the significance subpacket (and of the TDC subpacket) is **not signalled**; it is inferred
from the band widths (C.5.2 NOTE, C.5.6 NOTE; formula in 3.3 below). The subpacket lengths include
their padding and filler bytes.

Constraint when `Rl = 0` (C.3): `Dr[p,s]` must be identical for all packets of precinct p that
include the same band — raw/non-raw cannot be mixed within a band of a precinct (Figure C.1, p. 48).
When `Rl = 1` this restriction is lifted (CAP bit 8).

### 2.17 Packet body (C.4, Table C.5, p. 50) and subpackets (C.5)
```
if Isl > 0:  unpack_tdc_flags(p,s)        # Table C.11, size inferred
unpack_significance(p,s)                  # Table C.6, size inferred (may be empty)
unpack_bitplane_count(p,s)                # Table C.8, Lcnt bytes
unpack_data(p,s)                          # Table C.9, Ldat bytes
if Fs == 1: unpack_signs(p,s)             # Table C.10, Lsgn bytes
```
Each subpacket ends with `pad(8)`; bitplane-count, data and sign subpackets may additionally end with
filler bytes (`fill()`), whose count follows from the length fields (C.1). Field-level syntax of each
subpacket is given in Section 3 where it is decoded.

---

## 3. The decoding process end to end (Clause 7, Annexes A–H)

### 3.1 Data-flow (Figure 2, p. 14, with clause references)
```
codestream
  │
  ▼ [1] Annex A: parse SOC/CAP/PIH/CDT/WGT/…; derive bands (B.3/B.4), precinct grid (B.5), lines (B.6),
  │     packet inclusion I[p,λ,b,s] and Npc[p] (B.7 / Table B.4)
  ▼ slices (SLH/SLI + Ysl, Isl)            B.11: Np[t] precincts per slice
  ▼ precincts (precinct header, Table C.1)  → Q[p], R[p], D[p,b], (Qf, Rf, Di[p,b]), Lprc[p]
  │     T[p,b] = compute_truncation(b, Q[p], R[p], Di[p,b])            C.6.2 / Table C.12
  ▼ packets (packet header, Table C.4)      → Dr[p,s], Lcnt, Ldat, Lsgn
  ├─[2.5] TDC subpacket (C.5.6)             → Y[p,λ,b,k]   (SLI slices, Di = 3 bands only)
  ├─[2.1] significance subpacket (C.5.2)    → Z[p,λ,b,j]   (if D&2 and Dr = 0)
  ├─[2.2] bitplane-count subpacket (C.5.3, C.6)  → M[p,λ,b,g]   raw / no-pred / vertical, VLC of C.7
  ├─[2.3] data subpacket (C.5.4)            → v[p,λ,b,x]   (M−T bitplanes per code group; signs if Fs=0)
  └─[2.4] sign subpacket (C.5.5)            → s[p,λ,b,x]   (if Fs = 1)
  ▼ [3] Annex D dequantisation (Qpih: deadzone D.2 / uniform D.3)   → c[p,λ,b,x]
  ▼ [7] Annex H inverse temporal decorrelation (only Isl = 1)      → c'[p,λ,b,x]  (intra: c' = c)
  ▼ [4] Annex E inverse DWT (reorder+scale by 2^Fq E.3; 5/3 lifting E.7; order E.2) → O[c,x,y]
  ▼ [5] Annex F inverse multi-component transform (Cpih: none / RCT F.3 / Star-Tetrix F.5) → Ω[c,x,y]
  ▼ [6] Annex G DC shift, optional non-linearity, scale to B[i], clamp → R[c,x,y]
```
7.2: none of this ordering is mandatory; only the output is (within Part 4 error bounds). E.1 NOTE 1,
F.1 NOTE, G.1 NOTE, H.1 NOTE 2 all say a low-latency decoder should *interleave* these stages.

### 3.2 Stage 1 — from headers to layout
Everything below is computed once per codestream from PIH/CDT/CWD (formulas in Section 4):
band existence `b'x[b]`, `Wpb[p,b]`, `Ncg[p,b]`, `Ns[p,b]`, `L0/L1[p,b]`, `I[p,λ,b,s]`, `Npc[p]`.
Only the bottom precinct row differs from the others (fewer lines/bands, B.6 NOTE 2, A.2 NOTE 3) and the
rightmost column (narrower, B.5).

### 3.3 Significance subpacket (C.5.2, Table C.6, p. 51)
```
for b in 0..NL-1:
  for λ in L0[p,b]..L1[p,b]-1:
    if I[p,λ,b,s] and Dr[p,s] == 0 and (D[p,b] & 2):
      for j in 0..Ns[p,b]-1:  Z[p,λ,b,j] = u(1)
pad(8)
```
Size in bytes (C.5.3.2): `Lsig[p,s] = ⌈ Σ_{b,λ} I[p,λ,b,s]·(1−Dr[p,s])·(D[p,b]>>1)·Ns[p,b] / 8 ⌉`.

**Polarity:** in Tables C.15/C.16 the residual is decoded when `Z == 0`; `Z == 1` marks an
*insignificant* group (3.1.45: the subpacket "identifies which significance groups … are
insignificant"). What "insignificant" means depends on `Rm` (3.1.43, B.9): `Rm = 0` → all code groups
in the group have zero *bitplane-count prediction residual*; `Rm = 1` → all *coefficients* are zero.

### 3.4 Bitplane counts (C.5.3, C.6)

**Truncation position** (C.6.2, Table C.12, p. 60) — needed before decoding the counts:
```
compute_truncation(b, q, r, d):
    (pr, g) = (P[b], G[b]) if d != 1 else (Pr[b], Gr[b])      # d = Di[p,b] (0 in SLH slices)
    s = 1 if pr < r else 0                                     # one extra bitplane for high-priority bands
    return clamp(q − g − s, 0, 2^Br − 1)
T[p,b] = compute_truncation(b, Q[p], R[p], Di[p,b])
```
So `Q[p]` is a coarse per-precinct step (drop `Q` LSB bitplanes), each band keeps `G[b]` more bitplanes,
and bands whose priority `P[b] < R[p]` keep one more still. `R[p]` therefore acts as a fine rate knob
between two `Q` steps (with `P` a permutation of 0..NL−1, `R` in 0..NL). Bitplanes actually transmitted
for a code group: `max(M − T, 0)`.

**Vertical predictor** (C.6.3, Table C.13, p. 61) — `sy` = vertical sampling factor of band b's component:
```
compute_predictor(p, b, λ):
    for g: Mtop[p,λ,b,g] = M[p − Np,x, L1[p,b] − sy, b, g]  if λ − sy < L0[p,b]   # last line of precinct above
                         = M[p, λ − sy, b, g]                otherwise
    Ttop[p,b] = T[p − Np,x, b]  if λ − sy < L0[p,b]  else T[p,b]
```
Never used for the first precinct row of a slice (encoder must not select it) — the predictor always
stays inside the slice.

**Bitplane-count subpacket** (C.5.3.5, Table C.8, p. 55):
```
for b: for λ in L0..L1-1: if I[p,λ,b,s]:
    if Dr[p,s] == 1:            unpack_raw(p,b,λ)          # Table C.14
    elif (D[p,b] & 1) == 0:     unpack_nopred(p,b,λ)       # Table C.16
    else:                       unpack_vertical(p,b,λ)     # Table C.15
pad(8); fill()                                             # total = Lcnt[p,s] bytes
```
*Raw mode* (C.6.4, Table C.14): `M[p,λ,b,g] = u(Br)` for each of `Ncg[p,b]` code groups.

*No-prediction mode* (C.6.6, Table C.16, p. 63):
```
for g in 0..Ncg-1:
    mtop = T[p,b]
    if (D[p,b] & 2) == 0 or Z[p,λ,b,⌊g/Ss⌋] == 0:  Δm = vlc(mtop, T[p,b])
    else:                                          Δm = 0          # insignificant → M = T (nothing coded)
    M[p,λ,b,g] = mtop + Δm          # must land in 0..2^Br−1
```
*Vertical mode* (C.6.5, Table C.15, p. 62):
```
compute_predictor(p,b,λ)
for g:
    t    = max(T[p,b], Ttop[p,b])
    mtop = max(Mtop[p,λ,b,g], t)
    if (D[p,b] & 2) == 0 or Z[p,λ,b,⌊g/Ss⌋] == 0:  Δm = vlc(mtop, T[p,b])
    else:  Δm = 0 if Rm == 0 else T[p,b] − mtop     # Rm=0: zero residual; Rm=1: zero coefficients
    M[p,λ,b,g] = mtop + Δm
```
Note that `vlc()` is always called with `T[p,b]` (not `t`) as the truncation argument.

**VLC primitive** (C.7.1, Table C.17, p. 63–64): unary-coded with an alphabet switch at `θ = max(r − t, 0)`.
```
vlc(r, t):
    θ = max(r − t, 0); n = 0
    do { b = u(1); if b: n += 1 } while (b and n < 2^(Br+1))
    if n ≥ 2^(Br+1): error            # >32 (Br=4) / >64 (Br=5) consecutive 1-bits = lost sync
    if n > 2θ:   return n − θ         # unary sub-alphabet (only positive values)
    elif n > 0:  return −⌈n/2⌉ if n odd else ⌊n/2⌋   # signed sub-alphabet
    else:        return 0
```
Encoder inverse (C.7.2, Table C.18): `if x > θ: n = x + θ else: x2 = 2x; n = (−x2 − 1) if x2 < 0 else x2`;
emit `n` one-bits then a zero. Intuition: the predictor `r` can only be *under*-shot by at most `r − t`
(counts below `T` carry no data), so magnitudes ≤ θ get a sign-interleaved code and larger residuals
(necessarily positive) get plain unary.

**Rate constraints on the encoder** (C.5.3.1–C.5.3.4, Tables C.7, p. 52–55): the codestream shall be
built so that bitplane-count + significance bytes never exceed what raw coding (`Br` bits per code
group) would cost — summed per **band over the whole precinct** when `Rl = 0`, per **packet** when
`Rl = 1`. Purpose: bound decoder buffering for count data. Always satisfiable by choosing raw mode.
Table C.7 is the validation algorithm.

### 3.5 Data subpacket (C.5.4, Table C.9, p. 56–57)
```
for b: for λ in L0..L1-1: if I[p,λ,b,s]:
    for g in 0..Ncg[p,b]-1:
        v[p,λ,b,Ng·g+k] = 0 for k in 0..Ng-1
        if M[p,λ,b,g] > T[p,b]:
            if Fs == 0:  for k in 0..Ng-1: s[p,λ,b,Ng·g+k] = u(1)     # 4 sign bits first, incl. zero coefficients
            for i = M−T−1 down to 0:                                 # bitplane-major, MSB first
                for k in 0..Ng-1: v[p,λ,b,Ng·g+k] += u(1) << i
pad(8); fill()                                                       # total = Ldat[p,s] bytes
```
`v` is the **truncated** magnitude (the `T` LSB planes are not present; the dequantiser shifts back).
The number of bits of a code group is exactly `Ng·(M−T) (+Ng if Fs = 0)` — given `M` and `T`, the bit
offset of every code group is a prefix sum, so magnitude decoding is embarrassingly parallel
**[derived]**. The last code group of a line is padded to `Ng` coefficients; the decoder ignores the
padding coefficients downstream (B.8).

### 3.6 Sign subpacket (C.5.5, Table C.10, p. 57–58) — only if `Fs = 1`
```
for b: for λ: if I: for g: for k: if v[p,λ,b,Ng·g+k] != 0: s[p,λ,b,Ng·g+k] = u(1)
pad(8); fill()                                                       # total = Lsgn[p,s] bytes
```
Signs of padding coefficients are transmitted if they are non-zero (NOTE 2); encoders are advised to
zero them.

### 3.7 Dequantisation (Annex D) — `s = 1` means negative
Deadzone (`Qpih = 0`, D.2, Table D.1, p. 65): reconstruction point mid-bucket; zero bucket twice as wide.
```
if M[p,λ,b,g] > T[p,b] and v != 0:
    r = (1 << T) >> 1;  σ = 1 − 2·s
    c = σ · ((v << T) + r)
else: c = 0
```
Uniform (`Qpih = 1`, D.3, Table D.2, p. 66): all buckets equal width `Δ = 2^(M+1) / (2^(M+1−T) − 1)`,
implemented exactly with a shift-and-add Neumann series (float is *not* acceptable, D.3 NOTE):
```
if M > T and v != 0:
    σ = 1 − 2·s;  φ = v << T;  ζ = M − T + 1;  ρ = 0
    while φ > 0: ρ += φ; φ >>= ζ
    c = σ · ρ
else: c = 0
```
Encoder side (informative): deadzone `v = |c| >> T`, `s = (c < 0)` (D.4, Table D.3); uniform
`ζ = M−T+1; v = ((|c| << ζ) − |c| + (1 << M)) >> (M+1)` if `M > T` else 0 (D.5, Table D.4); bitplane
count `M` = number of bits of `max |c|` over the code group (D.6, Table D.5; `M = 0` for an all-zero group).
Inverse-quantised values are `c[p,λ,b,x]`, the "wavelet coefficient residuals"; in intra slices
`c' = c` (D.1 NOTE, A.4.14).

### 3.8 Inverse temporal decorrelation (Annex H) — only for SLI slices
The frame buffer `f[p,λ,b,x]` holds **wavelet coefficients**, not pixels (H.1 NOTE 1). Per slice
(Table H.1): if `Isl = 0` → `intra_copy` (`c' = c`, Table H.2); else `tdc_update(p, Q'f, R'f)`
(Table H.3) using the *previous* frame's `Qf/Rf` to dequantise `f`; then `update_framebuffer(p, Qf, Rf)`
requantises `c'` into `f` with the current `Qf/Rf` (Table H.4). Per coefficient (H.4, Table H.3):
```
Qad = max(Qbi, Qbr, 0);  k = ⌊x / (Si·Ng)⌋
if Di == 0 or Di == 1 or (Di == 3 and Y[p,λ,b,k] == 0):   c' = c << (Qad − Qbi)        # intra
elif Sh[b] ≥ 0 and ((k − Yh[b]) umod 2^Sh[b]) == 0:        c' = c << (Qad − Qbr)        # forced refresh
else:  T = compute_truncation(b, q, r, 0);  f' = f << T;   c' = (c << Qad) + f'          # predicted
```
Frame-buffer update: `f = c' >> T` (sign-symmetric), `T = compute_truncation(b, Qf, Rf, 0)`.
Initial frame-buffer content is undefined; a stream whose decoding depends on it is ill-formed, but
decoders "should handle such … gracefully" (H.4 NOTE 1–2). We do not plan to use TDC; the rest of these
notes assume `Isl = 0` unless stated.

### 3.9 Inverse DWT (Annex E)
**Decomposition structure** (E.2, Table E.1, p. 70–71; forward in E.9, Table E.8): per component `k`
with `N'L,x[k]` horizontal and `N'L,y[k]` vertical levels, `Dx = min(N'L,x, N'L,y)`:
1. Undo the horizontal-only levels first: for `dx = N'L,x` down to `Dx+1`:
   `hor_transform(LL_{dx−1,N'Ly} ← LL_{dx,N'Ly}, HL_{dx,N'Ly})`.
2. Then for each 2-D level `d = Dx` down to 1:
   `hor_transform(LL_{d−1,d} ← LL_{d,d}, HL_{d,d})`, `hor_transform(LH_{d−1,d} ← LH_{d,d}, HH_{d,d})`,
   `ver_transform(LL_{d−1,d−1} ← LL_{d−1,d}, LH_{d−1,d})`.
3. `O[k,x,y] = LL_{0,0}` (E.8, Table E.7).

I.e. the encoder does `N'L,y` 2-D levels (vertical then horizontal at each level) followed by
`N'L,x − N'L,y` horizontal-only levels on the LL band. The decoder inverts in reverse order; within a
2-D level the horizontal inverse runs first, then the vertical.

**Coefficient reordering and scaling** (E.3, Table E.2, p. 71–72): band-domain sample `(x, y)` of filter
type β in component k comes from
```
p = Np,x · ⌊ y·sy[k]·2^dy / 2^NL,y ⌋ + ⌊ x·sx[k]·2^dx / Cs ⌋
λ = y umod 2^(NL,y − log2 sy[k] − dy)        # band-local line; add L0[p,b] to get the Annex B/C line index (see 8.1)
ξ = x umod ⌈ Cs / (sx[k]·2^dx) ⌉
T[β,x,y] = c'[p,λ,b,ξ] << Fq                 # Fq fractional bits are appended here
```
Encoder inverse (E.14, Table E.13): `r = (1<<Fq)>>1; c' = (T + r) >> Fq` for `T ≥ 0`, `−((−T + r) >> Fq)`
otherwise (round-half-away-from-zero then drop `Fq` bits).

**1-D inverse 5/3 filter** (E.7, Table E.6, p. 74) on an interleaved array `X` (even = low-pass, odd =
high-pass; `X[x] = T[βL, ⌊x/2⌋]` for even x, `T[βH, ⌊x/2⌋]` for odd x — E.4/E.5) of length `Z`:
```
extend_samples(Z):  X[−i] = X[i]; X[Z+i−1] = X[Z−i−1]  for i = 1, 2        # whole-sample symmetric (E.6, Table E.5)
for i = 0, 2, 4, … < Z+1:  Y[i] = X[i] − ((X[i−1] + X[i+1] + 2) >> 2)      # even (low-pass) update
for i = 1, 3, 5, … < Z:    Y[i] = X[i] + ((Y[i−1] + Y[i+1]) >> 1)          # odd (high-pass) predict
```
Forward (E.13, Table E.12): `Y[odd] = X[odd] − ((X[i−1]+X[i+1]) >> 1)`, then
`Y[even] = X[even] + ((Y[i−1]+Y[i+1]+2) >> 2)`. This is the reversible integer LeGall 5/3 lifting used by
JPEG 2000. Bands are always ≥ 2 wide/high thanks to the PIH constraints (E.6 NOTE), so extension is
well defined. Horizontal (E.4, Table E.3) and vertical (E.5, Table E.4) passes are the same 1-D
routine applied to rows / columns. Low-pass band width is `⌈W/2⌉`, high-pass `⌊W/2⌋` (B.2), so for odd
lengths the last sample is a low-pass sample.

### 3.10 Inverse multi-component transform (Annex F)
Selection (F.2, Table F.1, p. 80):

| `Cpih` | Transform | Applies to | Preconditions |
|---|---|---|---|
| 0 | none, `Ω = O` | — | always allowed |
| 1 | inverse RCT (F.3) | components 0..2; `Ω = O` for c ≥ 3 | `Nc ≥ 3`, `sx = sy = 1` for c < 3 |
| 3 | inverse Star-Tetrix (F.5) | components 0..3; `Ω = O` for c ≥ 4 | `Nc ≥ 4`, `sx = sy = 1` for c < 4, CTS + CRG present |

*Inverse RCT* (Table F.2, p. 81): `i0,i1,i2 = O[0],O[1],O[2]` (Y, Cb, Cr);
`o1 = i0 − ((i1 + i2) >> 2)` (G); `o0 = o1 + i2` (R); `o2 = o1 + i1` (B); `Ω[0],Ω[1],Ω[2] = R,G,B`.
Forward (Table F.3): `Y = (R + 2G + B) >> 2`, `Cb = B − G`, `Cr = R − G`.

*Inverse Star-Tetrix* — see Section 5.

### 3.11 DC offset, non-linearity, clamping (Annex G)
Runs over **all** components including non-decomposed ones (G.3 NOTE). Selection by NLT marker
(Table G.1, p. 91):

*Linear* (no NLT; G.3, Table G.2, p. 92):
```
ζ = Bw − B[i];  m = (1 << B[i]) − 1
v = Ω[i,x,y] + ((1 << Bw) >> 1)             # inverse DC shift: coefficients are signed around 0
v = (v + ((1 << ζ) >> 1)) >> ζ              # drop Fq + (B-independent) extra bits with rounding
R[i,x,y] = clamp(v, 0, m)
```
Encoder (G.7, Table G.6): `Ω = (R << ζ) − ((1 << Bw) >> 1)`. So for `Bw = 20`, `B = 12`: `ζ = 8 = Fq`,
samples are scaled by 256 and offset by −2^19.

*Quadratic* (NLT `Tnlt = 1`; G.4, Table G.3, p. 92–93): `ζ = 2·Bw − B[i]`;
`v = clamp(Ω + 2^(Bw−1), 0, 2^Bw − 1); v = v·v; v = (v + 2^(ζ−1)) >> ζ; v += DCO; clamp(v, 0, m)`.
Intermediate needs up to 2^36 for `Bw = 18`. Encoder does an integer square root (G.8, Table G.7).

*Extended* (NLT `Tnlt = 2`; G.5, Table G.4, p. 93–94): three regions split at `T1`, `T2`: quadratic below
`T1` (black region), linear with slope `2^(Bw−E)` between, quadratic above; constants
`B2 = T1²`, `A1 = B2 + (T1 << (Bw−E)) + 2^(2Bw−2−2E)`, `B1 = T1 + 2^(Bw−E−1)`,
`A3 = B2 + (T2 << (Bw−E)) − 2^(2Bw−2−2E)`, `B3 = T2 − 2^(Bw−E−1)`, `ε = Bw − E`, `ζ = 2Bw − B[i]`.
Needs 2^38 intermediates. G.10 gives formulas to derive `T1`, `T2` from encoder-domain thresholds
`Θ1` (optical black), `Θ2` (gamma toe); `E = 3` suggested.

### 3.12 Encoder mirror (informative parts of the annexes)
Forward chain: G.6–G.9 input scaling → F.4 / F.6 forward RCT / Star-Tetrix → E.9–E.14 forward DWT and
insertion into precincts → D.6 bitplane counts, D.4/D.5 quantisation → (H.6–H.9 TDC) → Annex C packing,
with the encoder choosing `Q[p], R[p], D[p,b], Dr[p,s]` subject to the constraints in 3.4. No rate
allocation algorithm is specified anywhere in Part 1 (see 7.4).

---

## 4. Precinct, slice and band organisation (Annex B)

### 4.1 Components and sampling (B.1, p. 32–33)
`Wc[i] = ⌈Wf / sx[i]⌉`, `Hc[i] = ⌈Hf / sy[i]⌉`. The sampling grid is only an abstract coordinate system;
Part 1 does not say how to upsample or where subsampled samples sit (5.1, B.1 NOTE 1) — CRG may tell.

### 4.2 Per-component decomposition depth (B.2, p. 33)
```
for i <  Nc − Sd:  N'L,x[i] = NL,x;   N'L,y[i] = NL,y − log2(sy[i])
for i ≥  Nc − Sd:  N'L,x[i] = 0;      N'L,y[i] = 0                    # not decomposed (CWD)
```
Band dimensions of filter type β with depths `dx, dy` in component i:
`Wb = ⌈Wc / 2^dx⌉` (horizontal low-pass) or `⌊⌈Wc / 2^(dx−1)⌉ / 2⌋` (horizontal high-pass); `Hb` analogous.
With 0 vertical levels `Hb = Hc`.

### 4.3 Filter types β and band indices b (B.3, p. 33–35)
`Nβ = 2·min(NL,x, NL,y) + max(NL,x, NL,y) + 1`. With `τx = 1` for horizontal high-pass else 0, `τy`
likewise, for `NL,x ≥ NL,y`:
```
β = (NL,x − dx) + τx                                   if dx > dy      # horizontal-only levels
β = (NL,x − NL,y) + τx + 2·τy + 3·(NL,y − dy)          otherwise       # 2-D levels
```
(`NL,x < NL,y` is impossible given the PIH constraint `NL,y ≤ NL,x`.) Ordering is coarsest first. β does
not depend on sampling (B.3 NOTE 2); `dx[β,i], dy[β,i]` *do* depend on the component for vertically
subsampled components (NOTE 6). Reference tables (p. 36):

| β | Table B.1: 5h/0v | Table B.2: 5h/1v | Table B.3: 5h/2v |
|---|---|---|---|
| 0 | LL5,0 | LL5,1 | LL5,2 |
| 1 | HL5,0 | HL5,1 | HL5,2 |
| 2 | HL4,0 | HL4,1 | HL4,2 |
| 3 | HL3,0 | HL3,1 | HL3,2 |
| 4 | HL2,0 | HL2,1 | HL2,2 |
| 5 | HL1,0 | HL1,1 | LH2,2 |
| 6 | — | LH1,1 | HH2,2 |
| 7 | — | HH1,1 | HL1,1 |
| 8 | — | — | LH1,1 |
| 9 | — | — | HH1,1 |

(`XY d,e`: X = horizontal filter, Y = vertical filter, d = horizontal depth, e = vertical depth.)

Band count and index (component fastest, filter type slowest — B.3 NOTE 8):
```
NL = (Nc − Sd)·Nβ + Sd
b  = (Nc − Sd)·β + i                    for i < Nc − Sd
b  = (Nc − Sd)·Nβ + (i − (Nc − Sd))      for i ≥ Nc − Sd   # literally printed as "+ i"; see 8.1 — the examples (Tables B.10, B.11, I.9–I.11) require contiguous indices
```
Bands not populated: for vertically subsampled components (`sy > 1`) the filter types
`β = NL,x + 2 − NL,y` and `β = NL,x + 3 − NL,y` (the LH/HH of the deepest 2-D level) do not exist
(NOTE 3); for non-decomposed components only `β = 0` exists (NOTE 4). Figure B.2 (p. 34) shows the
4:2:0, 5h/2v layout: luma bands 0,3,6,…,27; Cb bands 1,4,7,10,13,22,25,28 (16, 19 missing); Cr likewise.

### 4.4 Band existence flags (B.4, p. 35)
```
bx[β,i] = 0  if β > 0 and i ≥ Nc − Sd
        = 0  if (2^max(NL,y − dy[β,i], 0) · τy[β]) umod sy[i] != 0      # first line of the band not divisible by sy
        = 1  otherwise
b'x[b] = bx[β,i] with b from 4.3
```
Needed to parse WGT/WGR and precinct headers (`D[p,b]` only for existing bands).

### 4.5 Precincts and columns (B.5, p. 37)
```
Cs   = 8·Cw·max_i(sx[i])·2^NL,x   if Cw > 0   else Wf        # column width in grid samples
Np,x = ⌈Wf / Cs⌉ ;  Np,y = ⌈Hf / 2^NL,y⌉ ;  Hp = 2^NL,y       # precinct height in grid lines
Wp[p] = Cs                         if p umod Np,x < Np,x − 1
      = ((Wf − 1) umod Cs) + 1     otherwise                  # rightmost column
```
Precinct indices `p` are raster order (left→right, top→bottom), `0 .. Np,x·Np,y − 1`. A "column"
(3.1.13) is a set of vertically aligned precincts. Coefficient `(xb, yb)` of band b belongs to
precinct p iff `⌊p / Np,x⌋ = ⌊yb·sy·2^dy / 2^NL,y⌋` and `p umod Np,x = ⌊xb·sx·2^dx / Cs⌋`.
Band width inside a precinct:
`Wpb[p,b] = ⌈Wp[p] / (sx·2^dx)⌉` (horizontal low-pass) or `⌊⌈Wp[p] / (sx·2^(dx−1))⌉ / 2⌋` (high-pass) —
this is also the number of quantisation indices per line. B.5 NOTE recommends choosing `Cw` so the
rightmost column is about as wide as the others.

### 4.6 Lines within a precinct (B.6, p. 38)
`λ ∈ [0, Hp − 1]` counts sampling-grid lines. Band b occupies lines `L0 ≤ λ < L1`:
```
L0[p,b] = 2^max(NL,y − dy, 0) · τy[β]
L1[p,b] = L0[p,b] + min( Hb[β,i] − ⌊p / Np,x⌋ · 2^max(NL,y − dy, 0),  2^max(NL,y − dy, 0) )
```
Example `NL,y = 2` (4 lines per precinct): LL/HL bands with `dy = 2` → 1 line at λ = 0; LH2,2/HH2,2 →
1 line at λ = 1; HL1,1 → lines 0,1; LH1,1/HH1,1 → lines 2,3 (Figure B.3, p. 39). For `sy = 2` components
only even λ are used (NOTE 1). The bottom precinct row may have fewer or zero lines in some bands
(NOTE 2).

### 4.7 Packets (B.7, Table B.4, p. 39–41)
A packet holds **one line λ of one or more bands**; all bands in a packet are coded jointly (share one
packet header and one set of subpackets). Algorithm (Table B.4), in words:
1. Packet 0: line 0 of filter types `β = 0 .. β1 − 1`, `β1 = max(NL,x,NL,y) − min(NL,x,NL,y) + 1`
   (the LL band plus all horizontal-only HL bands) for all decomposed components.
2. For each 2-D resolution level, coarsest first (`β0 = β1, β1+3, …`): for each line
   `λ' = 0 .. 2^(NL,y − dy[0,β0]) − 1`: for each of the three filter types `β0, β0+1, β0+2` (HL, LH,
   HH): one packet containing line `λ' + L0[p,b]` of that filter type for all decomposed components
   for which the band exists and `(λ' + L0) umod sy[i] == 0` and the line is inside the precinct.
   Empty packets are not created.
3. For each line `λ = 0 .. 2^NL,y − 1` and each non-decomposed component: one packet with its (only)
   band `β = 0`.
4. `Npc[p]` = number of packets created.

Reference layouts (Tables B.5–B.11, p. 41–43); band triples are `(comp0, comp1, comp2)`:

| Config | Packets `s`: line λ → bands |
|---|---|
| 5h/0v, 3 comp 4:4:4 (B.5) | s0: λ0 → (0,1,2)(3,4,5)(6,7,8)(9,10,11)(12,13,14)(15,16,17) — **one packet per precinct** |
| 5h/1v, 3 comp 4:4:4 (B.6) | s0: λ0 → 0..14; s1: λ0 → (15,16,17); s2: λ1 → (18,19,20); s3: λ1 → (21,22,23) |
| 5h/2v, 3 comp 4:4:4 (B.7) | s0: λ0 → 0..11; s1: λ0 → (12,13,14); s2: λ1 → (15,16,17); s3: λ1 → (18,19,20); s4: λ0 → (21,22,23); s5: λ2 → (24,25,26); s6: λ2 → (27,28,29); s7: λ1 → (21,22,23); s8: λ3 → (24,25,26); s9: λ3 → (27,28,29) |
| 5h/1v, 4:2:0 (B.8) | s0: λ0 → 0..14; s1: λ0 → (15,16,17); s2: λ1 → (18); s3: λ1 → (21) |
| 5h/2v, 4:2:0 (B.9) | as B.7 but chroma absent from odd lines and from bands 16,19: s2 → (15), s3 → (18), s7 → (21), s8 → (24), s9 → (27) |
| **5h/1v, 4 comp, `Sd = 1` (B.10)** | s0: λ0 → 0..14; s1: λ0 → (15,16,17); s2: λ1 → (18,19,20); s3: λ1 → (21,22,23); **s4: λ0 → (24); s5: λ1 → (24)** |
| **5h/2v, 4 comp, `Sd = 1` (B.11)** | s0..s9 as B.7; **s10: λ0 → (30); s11: λ1 → (30); s12: λ2 → (30); s13: λ3 → (30)** |

The last two rows are the layouts Annex I uses for CFA (Star-Tetrix with the Δ component
non-decomposed).

### 4.8 Code groups, significance groups, TDC groups (B.8–B.10, p. 43–44)
```
Ncg[p,b] = ⌈Wpb[p,b] / Ng⌉             # Ng = 4; last group padded with don't-care coefficients
Ns[p,b]  = ⌈Wpb[p,b] / (Ng·Ss)⌉        # Ss = 8 → 32 coefficients per significance group
Ni[p,b]  = ⌈Wpb[p,b] / (Ng·Si)⌉        # Si = 8 → 32 coefficients per TDC selection group
```

### 4.9 Slices (B.11, p. 44)
Slice `t` consists of `Np[t] = Np,x · h` precincts with `h = ⌈Hf/Hp⌉ umod Hsl` if
`(t+1)·Hsl > ⌈Hf/Hp⌉` (last slice) else `Hsl`. The first slice is aligned to the top. Entropy decoding
is independent between slices (no vertical prediction into a slice's first precinct row); the DWT is
**not** (Fslc = 0 only), so reconstructing pixels near a slice boundary needs coefficients from the
neighbouring slice (5.3, A.2, Table A.14).

### 4.10 Weights-table semantics (A.4.12, C.6.2)
`G[b]` (0..15): bitplanes of band b protected from truncation — `T = Q − G − s`. Typically largest for
LL/low-frequency bands (e.g. 4 for LL, 0 for HH1,1 in Table I.3) — it plays the role of the L2 synthesis
gain compensation for the unnormalised 5/3 lifting. `P[b]` (0..255): refinement priority — with
`R[p] > P[b]` band b gets one extra bitplane. Both come from the encoder and are simply obeyed by the
decoder. Annex I tables were optimised for PSNR; other choices may improve visual quality (A.4.12 NOTE).

---

## 5. Bayer / colour-filter-array specifics

### 5.1 Interpretation of CFA data (5.2, p. 12; Figure 1)
* A Bayer image is coded as a **4-component image on a super-pixel grid**: each sampling-grid point is a
  2x2 super pixel (3.1.49), so for the IMX676 `Wf = 3552/2 = 1776`, `Hf = 3556/2 = 1778`, `Nc = 4`,
  `sx = sy = 1` for all components **[derived]**.
* Component order is fixed **regardless of the physical arrangement**: component 0 = red, 1 and 2 =
  the two greens, 3 = blue (5.2; F.5.1 NOTE; Table F.4 output assignment). The physical sub-pixel
  position of each component is signalled by CRG (5.2, A.4.9).
* Alternatives that Part 1 also permits **[derived]**: `Cpih = 0` with four independent planes (no
  decorrelation, but trivially valid), or a 1-component grey-scale codestream of the raw mosaic
  (`Wf = 3552`, `Nc = 1`) — legal, but the DWT then sees the mosaic pattern as high-frequency content.
  Part 1's only *CFA-tuned* tool is Star-Tetrix.

### 5.2 Star-Tetrix (`Cpih = 3`; F.5, F.6; 3.1.47) — definition
A reversible integer lifting transform combining spatial and inter-component decorrelation of the four
Bayer planes. Coded (wavelet-domain) components `O[c]` and decoded outputs `Ω[c]`:

| Coded `O[c]` | Meaning (F.6.1 NOTE) | After inverse becomes `Ω[..]` |
|---|---|---|
| `O[0]` | `Ya` — average luma (Y2 corrected by mean Δ) | `Ω[2]` = G2 |
| `O[1]` | `Cb` — blue-green difference | `Ω[3]` = B |
| `O[2]` | `Cr` — red-green difference | `Ω[0]` = R |
| `O[3]` | `Δ` — differential luma `Y1 − mean(Y2)` | `Ω[1]` = G1 |

Forward (encoder guidance, Table F.13, p. 87, then Tables F.14–F.17): start with
`ω4[2] = R, ω4[3] = G1, ω4[0] = G2, ω4[1] = B`, then four lifting steps, each a full-image pass using
`access()` neighbours:
```
CbCr step (F.14):  Cb = B  − ⌊(Gl + Gr + Gt + Gb) / 4⌋            # 4-neighbour G mean at the B site
                   Cr = R  − ⌊(Gl + Gr + Gt + Gb) / 4⌋            # 4-neighbour G mean at the R site
Y step    (F.15):  Y2 = G2 + ⌊(2^e2·(Cb_l + Cb_r) + 2^e1·(Cr_t + Cr_b)) / 8⌋   # chroma neighbours of G2
                   Y1 = G1 + ⌊(2^e2·(Cb_t + Cb_b) + 2^e1·(Cr_l + Cr_r)) / 8⌋   # chroma neighbours of G1
delta step(F.16):  Δ  = Y1 − ⌊(Y2_lt + Y2_rt + Y2_lb + Y2_rb) / 4⌋   # diagonal Y2 neighbours
avg step  (F.17):  Ya = Y2 + ⌊(Δ_lt + Δ_rt + Δ_lb + Δ_rb) / 8⌋       # diagonal Δ neighbours
```
Inverse (normative, Table F.4, p. 82; F.5.2–F.5.5, Tables F.5–F.8): exactly the reverse order with
signs flipped — `inv_avg_step`: `Y2 = Ya − ⌊ΣΔdiag/8⌋`; `inv_delta_step`: `Y1 = Δ + ⌊ΣY2diag/4⌋`;
`inv_Y_step`: `G2 = Y2 − ⌊(2^e2(Cb_l+Cb_r) + 2^e1(Cr_t+Cr_b))/8⌋`,
`G1 = Y1 − ⌊(2^e2(Cb_t+Cb_b) + 2^e1(Cr_l+Cr_r))/8⌋`; `inv_CbCr_step`: `B = Cb + ⌊ΣG4/4⌋`,
`R = Cr + ⌊ΣG4/4⌋`. Because every step is a lifting step with floor, the transform is lossless in
integers; used with `Fq = 0` it supports lossless Bayer coding **[derived]**.

Which neighbour is "left of G2" etc. is resolved by the coordinate access function
(F.5.7, Table F.12, p. 86–87), which maps a sub-pixel offset `(rx, ry) ∈ {−1,0,+1}²` from the site of
coded component c at super pixel `(x, y)` to a `(component, x', y')` triple:
```
access(c, x, y, rx, ry):
    if 2x + rx + δx[c] < 0  or  2x + rx + δx[c] ≥ 2·Wf:      rx = −rx        # mirror at left/right image edge
    if (Cf == 3 and ry + δy[c] < 0) or (Cf == 3 and ry + δy[c] > 1)
       or 2y + ry + δy[c] < 0  or  2y + ry + δy[c] ≥ 2·Hf:   ry = −ry        # mirror at top/bottom edge, or ALWAYS stay in the super-pixel row when Cf = 3
    x' = ⌊(2x + rx + δx[c]) / 2⌋ ;  y' = ⌊(2y + ry + δy[c]) / 2⌋
    c' = k[(rx + δx[c]) umod 2, (ry + δy[c]) umod 2]
    return (c', x', y')
```
Parameters (CTS, 2.10): `e1, e2 ∈ 0..3` weight the two chroma differences in the luma estimate
(`e1 = e2 = 2` gives `Y = G + (mean Cb-pair + mean Cr-pair)/2`); `Cf = 0` full transform (needs the
super-pixel rows above and below — a 2-line context in sensor rows on each side); `Cf = 3` in-line: all
vertical accesses are reflected back into the current super-pixel row, so no line buffer is needed but
the diagonal Δ/avg steps and the vertical chroma neighbours degrade to same-row values.

### 5.3 CFA pattern signalling — `Ct`, `δ`, `k` (F.5.6, Tables F.9–F.11, p. 85–86)
`Ct` is derived from CRG (values of `Xcrg[c], Ycrg[c]` for output components c = 0..3 = R, G1, G2, B):

| Pattern | c0=R | c1=G1 | c2=G2 | c3=B | `Ct` | Note |
|---|---|---|---|---|---|---|
| **RGGB** | (0,0) | (32768,0) | (0,32768) | (32768,32768) | 0 | e1 = Cr weight, e2 = Cb weight |
| BGGR | (32768,32768) | (32768,0) | (0,32768) | (0,0) | 0 | e1 = Cb weight, e2 = Cr weight |
| GRBG | (32768,0) | (0,0) | (32768,32768) | (0,32768) | 1 | e1 = Cr weight, e2 = Cb weight |
| GBRG | (0,32768) | (0,0) | (32768,32768) | (32768,0) | 1 | e1 = Cb weight, e2 = Cr weight |
| anything else | | | | | reserved | |

So `Ct` only encodes whether the greens sit on the anti-diagonal (`Ct = 0`) or the main diagonal
(`Ct = 1`); swapping R and B does not change `Ct` but "can require exchanging e1 and e2" (F.5.6 NOTE),
because the transform itself only knows "the component horizontally adjacent to G2".

Displacement of *coded* component c inside the super pixel (Table F.10) and its inverse (Table F.11):

| coded c | role | `Ct = 0` `(δx,δy)` | `Ct = 1` `(δx,δy)` |
|---|---|---|---|
| 0 | Ya / G2 | (0,1) | (1,1) |
| 1 | Cb / B | (1,1) | (0,1) |
| 2 | Cr / R | (0,0) | (1,0) |
| 3 | Δ / G1 | (1,0) | (0,0) |

`k[δx,δy]` is the inverse table: `Ct = 0`: (0,1)→0, (1,1)→1, (0,0)→2, (1,0)→3; `Ct = 1`: (1,1)→0,
(0,1)→1, (1,0)→2, (0,0)→3. For the IMX676 RGGB **[derived]**: `Ct = 0`,
`Xcrg = [0, 32768, 0, 32768]`, `Ycrg = [0, 0, 32768, 32768]`, coded component 2 (Cr) is the top-left
sub-pixel, 3 (Δ) top-right, 0 (Ya) bottom-left, 1 (Cb) bottom-right.

### 5.4 Constraints and requirements for CFA coding (collected)
* `Cpih = 3` requires `Nc ≥ 4` and `sx = sy = 1` for the first four components (F.2, Table A.15);
  CAP bit 1 set (Table A.5); exactly one CTS (A.4.8) and exactly one CRG (A.4.9) present.
* No CFA-specific restriction on `NL,x`, `NL,y`, `Cw`, `Hsl` in Part 1; the general PIH constraints
  apply on the **super-pixel grid** (so `Hp = 2^NL,y` super-pixel rows = `2^(NL,y+1)` sensor rows).
  Profile-level limits (a Bayer profile, if any) are in Part 2 — not covered here.
* CWD with `Sd = 1` (component 3 = Δ not decomposed) is what all Annex I CFA examples use
  (Tables I.9–I.11, "Sd = 1"); it requires CAP bit 5 and is optional. With `Sd = 1` the Δ component
  costs one extra packet per precinct line (Tables B.10/B.11) and its band index is `NL − 1`.
* Weight tables differ between `Cf = 0` and `Cf = 3` (Annex I gives both columns).
* Output scaling (Annex G) treats the four planes like any components with `B[i] = 10 or 12`.

---

## 6. Bit depths, ranges and limits signalled in Part 1

| Quantity | Range | Reference |
|---|---|---|
| Component bit depth `B[i]` | 8..16 | Table A.15; A.2 |
| Components `Nc` | 1..8 | Table A.7 |
| `Wf`, `Hf` | up to 2^16 − 1; at least `max(sx)·2^NL,x` / `max(sy)·2^NL,y` | Table A.7 |
| Sampling factors | `sx ∈ {1,2}` only for components 1,2; `sy ∈ 1..sx` | Table A.15 |
| `NL,x` | 1..8 | Table A.7 |
| `NL,y` | `max_i log2 sy[i]` .. `min(NL,x, 6)` | Table A.7 |
| `Cw` | 0..2^16−1 with the modulo constraint | Table A.7 |
| `Hsl` | 1..2^16−1 precincts | Table A.7 |
| `Bw / Fq / Br` | (20,8,4), (18,6,4), (B[0],0,4) for B ≤ 12, (B[0],0,5) for B > 12 | Table A.8 |
| Bitplane count `M` | 0..2^Br − 1 (15 or 31) | C.6.5, C.6.6 |
| `Q[p]` | 0..31 (`Qf` 0..31, 32 = intra default) | Table C.1 |
| `R[p]` | 0..2·NL − 1 | Table C.1 |
| `G[b]` / `P[b]` | 0..15 / 0..255 | Table A.25 |
| `Lprc[p]` | 1..2^20 − 1 bytes (24-bit field) | Table C.1 |
| Short packet header | `Ldat ≤ 32767`, `Lcnt ≤ 8191`, `Lsgn ≤ 2047` | Table C.4 |
| Long packet header | `Ldat, Lcnt ≤ 2^20−1`, `Lsgn ≤ 32767` | Table C.4 |
| VLC run of 1-bits | < 2^(Br+1) (32 / 64) | C.7.1 |
| Wavelet coefficient domain | signed, nominal `Bw` bits incl. `Fq` fractional bits; DC offset `2^(Bw−1)` | G.2, G.3 |
| Output samples | clamped to `0 .. 2^B[i] − 1` | Table G.2 |
| Non-linear intermediates | up to 2^36 (quadratic) / 2^38 (extended) for `Bw = 18` | G.4/G.5 NOTEs |
| `Xcrg/Ycrg` | 0..65535 in 1/65536 grid units | Table A.21 |
| `Lcod` | 32-bit codestream length (CBR) | Table A.7 |

Profiles/levels/sublevels — hence maximum picture size, throughput, allowed `NL,y`, `Cw`, `Hsl`,
SLI usage, coded bit-rate — are **all in ISO/IEC 21122-2**; Part 1 only carries the `Ppih`/`Plev` fields
(0 = unrestricted).

For the IMX676 **[derived]**: RAW10 → `B = 10`, RAW12 → `B = 12`; both fit the regular
`Bw = 20, Fq = 8, Br = 4` lossy mode (`ζ = Bw − B` = 10 or 8 bit up-shift at the input) and the
lossless `Bw = B[0], Fq = 0, Br = 4` mode (`B ≤ 12`).

---

## 7. Latency and complexity notes (FPGA encoder, GPU decoder)

### 7.1 What the standard fixes about latency
* The unit of rate allocation and of independent coefficient coding is the **precinct = `2^NL,y`
  sampling-grid lines × one column** (B.5). With `NL,y = 2` on the super-pixel grid that is 4 grid lines
  = 8 sensor rows **[derived]**. `NL,y = 0` gives 1-line precincts (and then every precinct is a single
  packet, Table B.5).
* Encoder and decoder must know `Q[p], R[p]` (precinct header) before any packet of that precinct is
  emitted / parsed; `Lprc[p]` (precinct byte length) is also in the header, so the encoder must have
  finished coding the precinct before writing its header — i.e. **at least one precinct of buffering in
  the encoder**, unless the length is known by construction **[derived]**.
* Slices: `Hsl` precinct rows; a slice header carries only `Ysl`, so slices add no buffering by
  themselves; they bound the vertical-prediction dependency chain (C.6.3) and give resync points for
  the transport layer (A.4.14 NOTE).
* Channel buffer / rate models: explicitly out of scope of Part 1 (Clause 1, A.2 NOTE 2) — Part 2.

### 7.2 State carried across lines (decoder)
| Stage | State across lines/precincts | Reference |
|---|---|---|
| Bitplane counts | previous line's `M` per band (`Mtop`) and previous precinct's `T` per band (`Ttop`), *only* if vertical prediction is used; for the first precinct row of a slice nothing | C.6.3 |
| Data / sign decoding | none beyond the current packet | C.5.4, C.5.5 |
| Dequantisation | none | Annex D |
| Inverse DWT | vertical 5/3 lifting: 2 rows of context each side per level, applied recursively over `NL,y` levels; the DWT also spans slice boundaries (`Fslc = 0`) | E.5–E.7, A.2 |
| Star-Tetrix `Cf = 0` | one super-pixel row above and below (all four coded components) for the diagonal Δ/Ya steps and the vertical Cb/Cr neighbours; `Cf = 3` needs none | F.5.2–F.5.5, F.5.7 |
| Output scaling | none | Annex G |
| TDC (if used) | the whole previous frame in the wavelet domain, quantised with `Qf/Rf` | Annex H |

### 7.3 Parallelism (what is independent by construction)
* **Slices** are independent for everything up to and including dequantisation (B.11, C.6.1 NOTE).
  Only the IDWT (and Star-Tetrix `Cf = 0`) needs neighbouring-slice coefficients near the boundary.
* **Precinct columns** (`Cw > 0`) are independent for entropy decoding: the vertical predictor reads
  `p − Np,x`, i.e. the same column (Table C.13) **[derived]**. The DWT runs over the full width, so
  columns are not independent for reconstruction.
* **Within a column** precinct rows are sequential only for bands coded with vertical prediction
  (`D[p,b] & 1`). An encoder that never uses vertical prediction makes every precinct independently
  decodable at a small cost in count bits **[derived]**.
* **Within a precinct**: the packet headers must be read in order (each packet's offset is the sum of
  the previous `Lcnt + Ldat + Lsgn` plus the inferred significance/TDC sizes), but that is a few
  integers per packet. Once offsets are known, subpackets of different packets can be decoded in
  parallel.
* **Bitplane-count subpackets** are bit-serial (unary VLC, Table C.17) — the one genuinely sequential
  inner loop. Raw mode (`Dr = 1`, fixed `Br` bits per group) is random-access. Encoders may force raw
  mode per band (or per packet with `Rl = 1`) at a rate cost — a possible knob if VLC decoding limits
  GPU throughput **[derived]**.
* **Data subpacket**: given `M` and `T`, code-group bit offsets are a prefix sum of `Ng·(M − T) (+Ng)`
  (Table C.9) → one thread per code group **[derived]**. Sign subpacket likewise via prefix sum of
  non-zero counts (Table C.10).
* IDWT, MCT and output scaling are separable / pointwise and map directly onto CUDA.

### 7.4 Rate allocation is encoder-only and non-normative
Part 1 never specifies how to choose `Q[p]`, `R[p]`, `D[p,b]`, `Dr[p,s]`, `G[b]`, `P[b]`, `Qf/Rf` or the
TDC modes: Clause 6 ("Annex C to Annex G include informative subclauses that indicate how an encoder
may be implemented"), H.6 NOTE ("This International Standard does not provide guidance on how to arrive
at quantization values … Any choice is acceptable as long as the resulting codestream conforms to …
this document and ISO/IEC 21122-2"), A.4.12 NOTE (weights are examples). What *is* normative for the
encoder:
1. bitplane-count/significance bytes ≤ raw-mode bytes, per band-in-precinct (`Rl = 0`) or per packet
   (`Rl = 1`) — C.5.3.2/C.5.3.3, validation algorithm Table C.7;
2. `M ∈ 0..2^Br − 1` (C.6.5, C.6.6) — coefficients larger than `2^Br − 1` bitplanes cannot be coded
   (H.8 hints at this for TDC residuals);
3. no vertical prediction in the top precinct row of a slice (C.2, C.6.3);
4. `Dr` consistent per band within a precinct when `Rl = 0` (C.3);
5. `Lprc ≤ 2^20 − 1`; short-header field ranges (Table C.1, C.4);
6. `Lcod` correct for CBR (Table A.7).
The classic JPEG XS encoder strategy (compute all `M` for the precinct, then search `(Q, R)` for the
largest rate ≤ budget, since `T` is monotone in `Q` and `R`) is *implied* by the syntax but not written
down; H.9 describes the analogous search for `(Qf, Rf)` ("iterating over all possible pairs and
selecting the pair that provides the largest possible rate not exceeding the threshold") — the only
place in Part 1 where a rate-control procedure is sketched.

### 7.5 Encoder-side memory sketch for our sensor [derived]
`Wf = 1776`, `Nc = 4`, coefficients at `Bw = 20` bits: one precinct of coefficients (`NL,y = 2`, 4 grid
lines) ≈ 4·1776·4·20 bit ≈ 71 KB; the vertical 5/3 lifting adds ~2 lines of context per level per
component; Star-Tetrix `Cf = 0` adds one super-pixel row of 4 planes at 12 bit (≈ 10.7 KB). Raw sensor
data is 18.95 MB/frame at 12 bit (5.0 Gbit/s at 33 fps); Part 1 gives no compression-ratio guidance
beyond "typically 2:1 to 18:1" (Clause 1).

---

## 8. Open questions, ambiguities, and transcription list

### 8.1 Ambiguities and suspected errata noticed while reading
1. **Band index of non-decomposed components** (B.3 p. 35, Table B.4 p. 41, Table E.2 p. 71,
   Table E.13 p. 78): the printed formula `b = (Nc − Sd)·Nβ + i` for `i ≥ Nc − Sd` yields indices
   beyond `NL − 1` (e.g. `Nc = 4, Sd = 1, Nβ = 8` → `b = 27` but `NL = 25`). Tables B.10/B.11 and
   I.9–I.11 use the contiguous index `(Nc − Sd)·Nβ + (i − (Nc − Sd))`. Use the contiguous form;
   confirm against the Part 5 reference software.
2. **Line index λ**: Annex B/C index lines within the precinct with the band offset `L0[p,b]` (high-pass
   bands start at `2^(NL,y−dy)`), whereas Table E.2/E.13 compute `λ = y umod 2^(NL,y − log2 sy − dy)`
   (band-local, starting at 0). The two are related by `λ_C = λ_E + L0[p,b]`. Pick one convention
   internally.
3. **Significance flag polarity**: the pseudo-code (Tables C.15/C.16) decodes a residual when `Z == 0`,
   so `Z = 1` means *insignificant*; the prose in 7.1 ("indicates the presence of significant code
   groups") reads the other way. Follow the pseudo-code.
4. Table A.2 references the COM marker to A.4.10 (which is TPC); the extension marker is A.4.11.
5. B.3 NOTE 4 says "components `i > Nc − Sd`"; should be `≥` (B.2 and the band formulas use `≥`).
6. F.2 says "less than 4 components, i.e. if Nc < 3" — read `Nc < 4`. F.6.1 NOTE says "third component"
   twice — the differential luma Δ is the *fourth* coded component (index 3).
7. Table C.15 uses `t = max(T, Ttop)` to floor the predictor but passes `T[p,b]` (not `t`) to `vlc()`.
   Presumably intentional (θ then depends on `mtop − T`); verify against reference software.
8. In no-prediction mode (Table C.16) an insignificant group always gives `M = T` — the `Rm`
   distinction only matters in vertical mode (Table C.15).
9. Sign bits of zero-valued coefficients are transmitted in the data subpacket when `Fs = 0` (Table
   C.9 loops over all `Ng` coefficients). Their value is unconstrained; write 0.
10. `Qf[p] = 32` is the intra default although the field range is 0..31 (Table C.1) — it is a sentinel
    that makes `compute_truncation` clamp to `2^Br − 1` (everything truncated).
11. The short-header threshold is `Wf·Nc < 32752`, not 32768 (Table C.4) — just implement as written.
12. Annex I `P[b]` values are not always permutations (e.g. Table I.9 `Cf = 3` has 9 twice, Table
    I.10 `Cf = 3` has 9 twice) — may be misprints, but duplicates are syntactically legal (0..255).
13. The decoder is not required to check the C.5.3 encoder constraints (they are "shall be constructed"
    requirements on the codestream) — but a robust decoder should bound its count buffers accordingly.
14. How many coefficient lines of the neighbouring slice the IDWT needs (for slice-parallel
    reconstruction) is not stated; it follows from the 5/3 support (2 samples each side per level)
    and must be derived per `NL,y`.
15. **Part 2 is required before freezing parameters**: profile limits on `NL,y`, `Cw`, `Hsl`, `Ppih`,
    use of SLI/TDC, `Fs`, `Rl`, `Sd`, and any Bayer/CFA profile constraints live there (Table A.2,
    A.2 NOTE 2, H.9). Also Part 4 for the tolerated decoder error bound (7.2) — relevant if the CUDA
    decoder wants to deviate from the bit-exact integer algorithms.

### 8.2 Tables and figures an implementer must transcribe (printed page = PDF page − 4)
| Item | Page | Needed for |
|---|---|---|
| Table A.2 marker codes | 19 | parser |
| Table A.5 CAP bits; Table A.6 CAP syntax | 21 | parser |
| Table A.7 PIH syntax (2 pages); Table A.8 Bw/Fq/Br combos; Tables A.9–A.14 enums | 22–24 | parser |
| Table A.15 CDT | 24 | parser |
| Tables A.16–A.17 NLT | 25 | parser (if NLT used) |
| Table A.18 CWD; Tables A.19–A.20 CTS | 26 | parser, CFA |
| Table A.21 CRG; Table A.22 TPC | 27–28 | parser, CFA (TPC only for TDC) |
| Tables A.23–A.24 COM | 28–29 | parser |
| Tables A.25–A.26 WGT/WGR | 29–30 | parser |
| Tables A.27–A.28 SLH/SLI | 30–31 | parser |
| B.2–B.6 formulas; Tables B.1–B.3 filter types; Figure B.2 | 33–38, 34, 36 | band/precinct geometry |
| Table B.4 `compute_packet_inclusion` (2 pages); Tables B.5–B.11 (checks) | 40–43 | packet layout |
| Table C.1 precinct header; Tables C.2–C.3 modes | 46–48 | entropy decoder |
| Table C.4 packet header | 49–50 | entropy decoder |
| Table C.5 packet body; Table C.6 significance; Table C.8 count subpacket | 50–51, 55 | entropy decoder |
| Table C.7 validity check | 54–55 | encoder |
| Table C.9 data subpacket; Table C.10 sign subpacket; Table C.11 TDC subpacket | 56–59 | entropy decoder |
| Table C.12 truncation; Table C.13 predictor; Tables C.14–C.16 raw/vertical/no-pred | 60–63 | entropy decoder |
| Table C.17 `vlc()`; Table C.18 `vlc_encode()` | 63–64 | both |
| Tables D.1–D.2 inverse quantisers; Tables D.3–D.5 forward + bitplane count | 65–69 | both |
| Table E.1 IDWT order; Table E.2 reorder/scale; Tables E.3–E.7 filters/extension | 70–75 | decoder |
| Tables E.8–E.13 forward DWT and insertion | 75–79 | encoder |
| Table F.1 selection; Table F.2/F.3 RCT | 80–81 | RGB path |
| Tables F.4–F.8 inverse Star-Tetrix; Tables F.9–F.11 CFA tables; Table F.12 `access()` | 82–87 | CFA decoder |
| Tables F.13–F.17 forward Star-Tetrix | 87–90 | CFA encoder |
| Table G.1 selection; Table G.2 linear; Tables G.3–G.4 non-linear | 91–94 | decoder |
| Tables G.5–G.8 input scaling; G.10 formulas | 94–97 | encoder |
| Tables H.1–H.6 frame buffer | 99–104 | only if TDC |
| Tables I.1–I.11 example weights | 106–113 | encoder defaults (see Appendix A) |
| Figure 2 decoder overview | 14 | orientation |
| Figure B.3 precincts/slices/packets; Figure C.1 raw-mode flags; Figure C.2 rate constraint | 39, 48, 53 | orientation |

---

## Appendix A — Example weight tables relevant to us (Annex I, informative)
Transcribed from the rendered PDF; **verify against the PDF before hard-coding.** `G` = gain, `P` = priority.

**Table I.1 — 4:4:4, RCT, 5h/0v (18 bands), p. 106**
`(b: G,P)` 0: 3,6 · 1: 2,8 · 2: 2,7 · 3: 2,1 · 4: 1,4 · 5: 1,5 · 6: 2,12 · 7: 1,14 · 8: 1,15 · 9: 1,0 ·
10: 0,2 · 11: 0,3 · 12: 1,9 · 13: 0,11 · 14: 0,10 · 15: 1,13 · 16: 0,16 · 17: 0,17

**Table I.3 — 4:4:4, RCT, 5h/2v (30 bands), p. 107–108**
0: 4,12 · 1: 3,15 · 2: 3,14 · 3: 3,3 · 4: 2,11 · 5: 2,10 · 6: 3,24 · 7: 2,26 · 8: 2,27 · 9: 2,0 ·
10: 1,4 · 11: 1,5 · 12: 2,18 · 13: 1,21 · 14: 1,20 · 15: 2,19 · 16: 1,23 · 17: 1,22 · 18: 1,13 ·
19: 0,16 · 20: 0,17 · 21: 1,2 · 22: 0,9 · 23: 0,6 · 24: 1,1 · 25: 0,7 · 26: 0,8 · 27: 1,25 · 28: 0,28 · 29: 0,29

**Table I.9 — CFA, Star-Tetrix, `Sd = 1`, 5h/0v (19 bands), p. 111**

| b | Cf=0 G | Cf=0 P | Cf=3 G | Cf=3 P |
|---|---|---|---|---|
| 0 | 3 | 3 | 4 | 18 |
| 1 | 3 | 17 | 3 | 17 |
| 2 | 3 | 16 | 3 | 16 |
| 3 | 2 | 1 | 3 | 13 |
| 4 | 2 | 15 | 2 | 12 |
| 5 | 2 | 14 | 2 | 11 |
| 6 | 2 | 11 | 2 | 5 |
| 7 | 1 | 7 | 1 | 4 |
| 8 | 1 | 6 | 1 | 3 |
| 9 | 1 | 0 | 2 | 14 |
| 10 | 1 | 13 | 1 | 10 |
| 11 | 1 | 12 | 1 | 9 |
| 12 | 1 | 10 | 1 | 6 |
| 13 | 0 | 5 | 0 | 1 |
| 14 | 0 | 4 | 0 | 0 |
| 15 | 1 | 18 | 1 | 15 |
| 16 | 0 | 9 | 0 | 9 |
| 17 | 0 | 8 | 0 | 7 |
| 18 (Δ, no DWT) | 0 | 2 | 0 | 2 |

**Table I.10 — CFA, Star-Tetrix, `Sd = 1`, 5h/1v (25 bands), p. 112**

| b | Cf=0 G | Cf=0 P | Cf=3 G | Cf=3 P |
|---|---|---|---|---|
| 0 | 4 | 20 | 4 | 13 |
| 1 | 3 | 17 | 3 | 12 |
| 2 | 3 | 16 | 3 | 11 |
| 3 | 3 | 12 | 3 | 8 |
| 4 | 2 | 11 | 2 | 7 |
| 5 | 2 | 10 | 2 | 6 |
| 6 | 2 | 0 | 3 | 20 |
| 7 | 2 | 24 | 2 | 19 |
| 8 | 2 | 23 | 2 | 18 |
| 9 | 2 | 15 | 2 | 9 |
| 10 | 1 | 9 | 1 | 5 |
| 11 | 1 | 8 | 1 | 4 |
| 12 | 1 | 1 | 2 | 21 |
| 13 | 1 | 22 | 1 | 17 |
| 14 | 1 | 21 | 1 | 16 |
| 15 | 1 | 14 | 1 | 10 |
| 16 | 0 | 6 | 0 | 2 |
| 17 | 0 | 4 | 0 | 1 |
| 18 | 1 | 13 | 1 | 9 |
| 19 | 0 | 5 | 1 | 23 |
| 20 | 0 | 3 | 1 | 22 |
| 21 | 0 | 7 | 1 | 24 |
| 22 | 0 | 19 | 0 | 15 |
| 23 | 0 | 18 | 0 | 14 |
| 24 (Δ, no DWT) | 0 | 2 | 0 | 3 |

**Table I.11 — CFA, Star-Tetrix, `Sd = 1`, 5h/2v (31 bands), p. 112–113**

| b | Cf=0 G | Cf=0 P | Cf=3 G | Cf=3 P |
|---|---|---|---|---|
| 0 | 4 | 9 | 4 | 5 |
| 1 | 3 | 8 | 3 | 4 |
| 2 | 3 | 7 | 3 | 3 |
| 3 | 3 | 0 | 4 | 28 |
| 4 | 3 | 30 | 3 | 27 |
| 5 | 3 | 29 | 3 | 26 |
| 6 | 3 | 20 | 3 | 18 |
| 7 | 2 | 19 | 2 | 17 |
| 8 | 2 | 18 | 2 | 16 |
| 9 | 2 | 1 | 3 | 30 |
| 10 | 2 | 28 | 2 | 23 |
| 11 | 2 | 27 | 2 | 22 |
| 12 | 2 | 24 | 2 | 19 |
| 13 | 1 | 16 | 1 | 14 |
| 14 | 1 | 14 | 1 | 13 |
| 15 | 2 | 23 | 2 | 12 |
| 16 | 1 | 15 | 1 | 11 |
| 17 | 1 | 13 | 1 | 10 |
| 18 | 1 | 17 | 1 | 9 |
| 19 | 0 | 11 | 0 | 8 |
| 20 | 0 | 10 | 0 | 7 |
| 21 | 1 | 22 | 1 | 15 |
| 22 | 0 | 6 | 0 | 2 |
| 23 | 0 | 4 | 0 | 1 |
| 24 | 1 | 21 | 1 | 0 |
| 25 | 0 | 5 | 1 | 25 |
| 26 | 0 | 3 | 1 | 24 |
| 27 | 0 | 12 | 1 | 29 |
| 28 | 0 | 26 | 0 | 21 |
| 29 | 0 | 25 | 0 | 20 |
| 30 (Δ, no DWT) | 0 | 2 | 0 | 6 |

Band → (component, filter) for these tables: `b = 3·β + i`, i ∈ {0: Ya, 1: Cb, 2: Cr}, β per Table
B.1/B.2/B.3; the last band is component 3 (Δ). Not transcribed: I.2 (4:4:4 5h/1v), I.4–I.6 (4:2:2),
I.7–I.8 (4:2:0), p. 106–111.

---

## Appendix B — Worked parameter set for one IMX676 stream [derived, not from the standard]
Assumptions: RAW12 RGGB, 3552×3556, Star-Tetrix `Cf = 0`, `Sd = 1`, `NL,x = 5`, `NL,y = 2`, no TDC, no NLT.

| Parameter | Value | Why |
|---|---|---|
| `Wf × Hf` | 1776 × 1778 | super-pixel grid (5.2) |
| `Nc`, `B[c]`, `sx`, `sy` | 4, 12, 1, 1 | Table A.15, F.2 |
| `Bw, Fq, Br` | 20, 8, 4 | regular row of Table A.8 (`Fq = 0, Bw = 12` for lossless) |
| `Cpih`, CTS | 3; `Cf = 0`, `e1 = e2 = ?` (tune) | Table A.9, A.19 |
| CRG | `Xcrg = [0, 32768, 0, 32768]`, `Ycrg = [0, 0, 32768, 32768]` → `Ct = 0` | Table F.9 RGGB row |
| CAP bits | 1 (Star-Tetrix), 5 (CWD); + 6 if lossless, + 8 if `Rl = 1` | Table A.5 |
| `Nβ`, `NL` | 10, 3·10 + 1 = 31 | B.3 |
| Packets per precinct | 14 (Table B.11) | B.7 |
| `Hp`, `Np,y` | 4 grid lines (8 sensor rows); ⌈1778/4⌉ = 445 (last precinct row has 2 lines) | B.5 |
| `Cw` options | 0 → one column of 1776; 1 → `Cs = 256`, 7 columns, last 240 wide; 2 → 512, 4 columns; 3 → 768, 3 columns (all satisfy `Wf umod Cs ≥ 32`) | Table A.7, B.5 |
| Precinct header | 24 + 8 + 8 + 2·31 = 102 bits → 13 bytes | Table C.1 |
| Packet header | short (`Wf·Nc = 7104 < 32752`, `Lh = 0`): 5 bytes × 14 = 70 bytes/precinct | Table C.4 |
| Weights | Table I.11, `Cf = 0` columns, as starting point | Annex I |
| Uncompressed | 18.95 MB/frame, 5.0 Gbit/s at 33 fps | — |

