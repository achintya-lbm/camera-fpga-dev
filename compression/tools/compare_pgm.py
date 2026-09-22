#!/usr/bin/env python3
"""PSNR / error statistics between two 16-bit PGM images (e.g. original mosaic vs decoded).

    compression/tools/compare_pgm.py <reference.pgm> <test.pgm> [--bits 10] [--codestream file.jxs]
Exit status 0 when identical, 1 otherwise (so it doubles as a bit-exactness check).
"""
import argparse, os, sys
import numpy as np

def read_pgx(path):
    """ISO conformance reference: <name>.pgx lists the raw file; <name>.pgx_0.h holds 'PG ML +12 976 650'."""
    base = path[:-4] if path.endswith(".pgx") else path
    header = open(base + ".pgx_0.h").read().split()
    endian = ">" if header[1].startswith("M") else "<"
    depth = int(header[2].lstrip("+-")); w, h = int(header[3]), int(header[4])
    raw = os.path.join(os.path.dirname(path), open(path).read().split()[0])
    data = np.fromfile(raw, dtype=endian + ("u2" if depth > 8 else "u1")).reshape(h, w)
    return data.astype(np.int64), (1 << depth) - 1

def read_pgm(path):
    if path.endswith(".pgx"):
        return read_pgx(path)
    with open(path, "rb") as f:
        magic = f.readline().strip()
        if magic != b"P5": sys.exit(f"{path}: not a binary PGM")
        dims = f.readline().split()
        while len(dims) < 2: dims += f.readline().split()
        maxval = int(f.readline()); w, h = int(dims[0]), int(dims[1])
        data = np.frombuffer(f.read(), dtype=">u2" if maxval > 255 else "u1").reshape(h, w)
    return data.astype(np.int64), maxval

ap = argparse.ArgumentParser()
ap.add_argument("reference"); ap.add_argument("test")
ap.add_argument("--codestream", help="report bit/pixel and ratio for this file")
ap.add_argument("--bits", type=int, help="bit depth for PSNR peak (default: from the PGM maxval)")
args = ap.parse_args()
ref, maxval = read_pgm(args.reference)
test, _ = read_pgm(args.test)
if ref.shape != test.shape: sys.exit(f"size mismatch {ref.shape} vs {test.shape}")
peak = (1 << args.bits) - 1 if args.bits else maxval
err = test - ref; mse = float((err ** 2).mean())
psnr = 10 * np.log10(peak ** 2 / mse) if mse > 0 else float("inf")
line = f"PSNR={psnr:.2f} dB max|err|={int(np.abs(err).max())} rms={np.sqrt(mse):.3f} identical={mse == 0}"
if args.codestream:
    size = os.path.getsize(args.codestream)
    line += f" | {8 * size / ref.size:.3f} bit/pixel, {size} bytes"
names = {"R": (0, 0), "G1": (0, 1), "G2": (1, 0), "B": (1, 1)}
chans = "  ".join(f"{n}: rms={np.sqrt((err[dy::2, dx::2] ** 2).mean()):.3f} max={int(np.abs(err[dy::2, dx::2]).max())}" for n, (dy, dx) in names.items())
print(line); print("  per Bayer site:", chans)
sys.exit(0 if mse == 0 else 1)
