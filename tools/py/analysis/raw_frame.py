#!/usr/bin/env python3
"""Decode raw CSI-2 frames dumped by bandwidth_test (--dump-dir) into images.

Each dump is `<camera>_<frame>.raw` plus a JSON sidecar with width/height/pixel_format/line_bytes.
RAW10 is unpacked from the CSI-2 5-bytes-per-4-pixels packing, RAW12 from 3-bytes-per-2-pixels.
The Bayer mosaic (RGGB) is demosaiced with a simple half-resolution "superpixel" method for
previews (fast, no colour fringes) and optionally at full resolution with bilinear interpolation.
Output: sRGB PNG/JPEG previews, a 1:1 centre crop, and per-channel statistics.

  bazel run //tools/py:raw_frame -- captures/cam3-J1D_000123.raw --out docs/samples/imx676 \
      --preview-width 1600 --crop 800 --gain auto
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image


def unpack_raw10(line: np.ndarray, width: int) -> np.ndarray:
    """CSI-2 RAW10: 4 pixels in 5 bytes (4 MSB bytes, then one byte with the 4 x 2 LSBs)."""
    groups = width // 4
    b = line[: groups * 5].reshape(groups, 5).astype(np.uint16)
    out = np.empty((groups, 4), dtype=np.uint16)
    for i in range(4):
        out[:, i] = (b[:, i] << 2) | ((b[:, 4] >> (2 * i)) & 0x3)
    return out.reshape(-1)[:width]


def unpack_raw12(line: np.ndarray, width: int) -> np.ndarray:
    """CSI-2 RAW12: 2 pixels in 3 bytes (2 MSB bytes, then one byte with the 2 x 4 LSBs)."""
    groups = width // 2
    b = line[: groups * 3].reshape(groups, 3).astype(np.uint16)
    out = np.empty((groups, 2), dtype=np.uint16)
    out[:, 0] = (b[:, 0] << 4) | (b[:, 2] & 0xF)
    out[:, 1] = (b[:, 1] << 4) | (b[:, 2] >> 4)
    return out.reshape(-1)[:width]


def load_frame(raw_path: pathlib.Path, sidecar: dict) -> tuple[np.ndarray, int]:
    width, height = int(sidecar["width"]), int(sidecar["height"])
    bits = int(sidecar.get("bits", 10))
    line_bytes = int(sidecar["line_bytes"])
    start = int(sidecar.get("start_byte", 0))
    data = np.fromfile(raw_path, dtype=np.uint8)
    needed = start + line_bytes * height
    if data.size < needed:
        raise SystemExit(f"{raw_path}: {data.size} bytes, expected at least {needed}")
    lines = data[start : start + line_bytes * height].reshape(height, line_bytes)
    unpack = unpack_raw10 if bits == 10 else unpack_raw12
    img = np.empty((height, width), dtype=np.uint16)
    for y in range(height):
        img[y] = unpack(lines[y], width)
    return img, bits


def bayer_stats(img: np.ndarray, bits: int) -> dict:
    r, g1, g2, b = img[0::2, 0::2], img[0::2, 1::2], img[1::2, 0::2], img[1::2, 1::2]
    full = (1 << bits) - 1
    return {
        "bits": bits,
        "mean_r": float(r.mean()),
        "mean_g": float((g1.mean() + g2.mean()) / 2),
        "mean_b": float(b.mean()),
        "min": int(img.min()),
        "max": int(img.max()),
        "p01": int(np.percentile(img, 1)),
        "p99": int(np.percentile(img, 99)),
        "saturated_fraction": float((img >= full - 1).mean()),
    }


def demosaic_half(img: np.ndarray) -> np.ndarray:
    """RGGB 2x2 superpixel -> RGB float image at half resolution."""
    r = img[0::2, 0::2].astype(np.float32)
    g = (img[0::2, 1::2].astype(np.float32) + img[1::2, 0::2].astype(np.float32)) / 2
    b = img[1::2, 1::2].astype(np.float32)
    h = min(r.shape[0], g.shape[0], b.shape[0])
    w = min(r.shape[1], g.shape[1], b.shape[1])
    return np.dstack([r[:h, :w], g[:h, :w], b[:h, :w]])


def demosaic_bilinear(img: np.ndarray) -> np.ndarray:
    """RGGB bilinear demosaic at full resolution (float32 RGB)."""
    h, w = img.shape
    f = img.astype(np.float32)
    pad = np.pad(f, 1, mode="reflect")
    out = np.zeros((h, w, 3), dtype=np.float32)
    yy, xx = np.mgrid[0:h, 0:w]
    red_site = (yy % 2 == 0) & (xx % 2 == 0)
    blue_site = (yy % 2 == 1) & (xx % 2 == 1)
    green_site = ~(red_site | blue_site)
    # neighbourhood averages
    cross = (pad[:-2, 1:-1] + pad[2:, 1:-1] + pad[1:-1, :-2] + pad[1:-1, 2:]) / 4
    diag = (pad[:-2, :-2] + pad[:-2, 2:] + pad[2:, :-2] + pad[2:, 2:]) / 4
    horiz = (pad[1:-1, :-2] + pad[1:-1, 2:]) / 2
    vert = (pad[:-2, 1:-1] + pad[2:, 1:-1]) / 2
    # green
    out[..., 1] = np.where(green_site, f, cross)
    # red
    out[..., 0] = np.where(red_site, f, np.where(blue_site, diag, np.where(yy % 2 == 0, horiz, vert)))
    # blue
    out[..., 2] = np.where(blue_site, f, np.where(red_site, diag, np.where(yy % 2 == 1, horiz, vert)))
    return out


def to_srgb8(rgb: np.ndarray, bits: int, black: float, gain: str | float, wb: bool) -> np.ndarray:
    full = float((1 << bits) - 1)
    x = np.clip(rgb - black, 0, None) / max(full - black, 1.0)
    if wb:
        # Grey-world white balance to green over pixels that are neither clipped nor black.
        flat = x.reshape(-1, 3)
        valid = (flat.max(axis=1) < 0.9) & (flat.min(axis=1) > 0.005)
        sample = flat[valid] if valid.sum() > 1000 else flat
        means = np.maximum(sample.mean(axis=0), 1e-6)
        x = x * (means[1] / means)
    if gain == "auto":
        p = np.percentile(x, 99.5)
        g = 0.9 / max(p, 1e-6)
    else:
        g = float(gain)
    x = np.clip(x * g, 0, 1)
    srgb = np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)
    return (srgb * 255 + 0.5).astype(np.uint8)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("raw", type=pathlib.Path, nargs="+", help=".raw dump(s); the .json sidecar sits next to it")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("."), help="output directory")
    ap.add_argument("--preview-width", type=int, default=1600, help="width of the JPEG preview (0 = skip)")
    ap.add_argument("--crop", type=int, default=800, help="side of the 1:1 centre crop PNG (0 = skip)")
    ap.add_argument("--full-png", action="store_true", help="also write a full-resolution 8-bit PNG (large)")
    ap.add_argument("--black", type=float, default=None, help="black level in sensor units (default 50 or 200)")
    ap.add_argument("--gain", default="auto", help="linear gain before gamma, or 'auto' (99.5th percentile -> 0.9)")
    ap.add_argument("--no-wb", action="store_true", help="skip grey-world white balance")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    for raw_path in args.raw:
        side_path = raw_path.with_suffix(".json")
        if not side_path.exists():
            print(f"missing sidecar {side_path}", file=sys.stderr)
            return 2
        sidecar = json.loads(side_path.read_text())
        img, bits = load_frame(raw_path, sidecar)
        black = args.black if args.black is not None else (200.0 if bits == 12 else 50.0)
        stats = bayer_stats(img, bits)
        stem = raw_path.stem
        outputs = {}
        if args.preview_width > 0:
            half = demosaic_half(img)
            rgb8 = to_srgb8(half, bits, black, args.gain, not args.no_wb)
            im = Image.fromarray(rgb8)
            if im.width > args.preview_width:
                im = im.resize((args.preview_width, round(im.height * args.preview_width / im.width)), Image.LANCZOS)
            p = args.out / f"{stem}_preview.jpg"
            im.save(p, quality=90)
            outputs["preview"] = str(p)
        if args.crop > 0:
            h, w = img.shape
            c = min(args.crop, h - h % 2, w - w % 2)
            y0 = (h - c) // 2 // 2 * 2
            x0 = (w - c) // 2 // 2 * 2
            crop = demosaic_bilinear(img[y0 : y0 + c, x0 : x0 + c])
            p = args.out / f"{stem}_center{c}.png"
            Image.fromarray(to_srgb8(crop, bits, black, args.gain, not args.no_wb)).save(p)
            outputs["center_crop"] = str(p)
        if args.full_png:
            full = demosaic_bilinear(img)
            p = args.out / f"{stem}_full.png"
            Image.fromarray(to_srgb8(full, bits, black, args.gain, not args.no_wb)).save(p)
            outputs["full"] = str(p)
        report = {"raw": str(raw_path), **{k: sidecar[k] for k in ("mode", "width", "height", "pixel_format", "fps") if k in sidecar},
                  "stats": stats, "outputs": outputs}
        print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
