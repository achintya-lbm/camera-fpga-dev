#!/usr/bin/env python3
"""Unpack a captured CSI RAW10/RAW12 frame (+ JSON sidecar from FrameCheckOp/SnapshotOp) into a
16-bit binary PGM Bayer mosaic, the input format of the JPEG XS reference encoder and of jxs_encode.

    compression/tools/raw_to_pgm.py <frame.raw> [frame.json] [-o out.pgm]
"""
import argparse, json, os, sys
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument("raw")
ap.add_argument("sidecar", nargs="?")
ap.add_argument("-o", "--out")
args = ap.parse_args()
sidecar = args.sidecar or os.path.splitext(args.raw)[0] + ".json"
side = json.load(open(sidecar))
w, h, lb, sb, bits = side["width"], side["height"], side["line_bytes"], side.get("start_byte", 0), side["bits"]
raw = np.fromfile(args.raw, dtype=np.uint8)
rows = raw[sb:sb + lb * h].reshape(h, lb)
if bits == 10:
    n = w // 4
    b = rows[:, :n * 5].reshape(h, n, 5).astype(np.uint16)
    px = np.empty((h, n, 4), dtype=np.uint16)
    for k in range(4):
        px[..., k] = (b[..., k] << 2) | ((b[..., 4] >> (2 * k)) & 3)
elif bits == 12:
    n = w // 2
    b = rows[:, :n * 3].reshape(h, n, 3).astype(np.uint16)
    px = np.empty((h, n, 2), dtype=np.uint16)
    px[..., 0] = (b[..., 0] << 4) | (b[..., 2] & 0xF)
    px[..., 1] = (b[..., 1] << 4) | (b[..., 2] >> 4)
else:
    sys.exit(f"unsupported bit depth {bits}")
img = px.reshape(h, w)
out = args.out or os.path.splitext(args.raw)[0] + ".pgm"
with open(out, "wb") as f:
    f.write(f"P5\n{w} {h}\n{(1 << bits) - 1}\n".encode())
    f.write(img.astype(">u2").tobytes())
print(f"{out}: {w}x{h} {bits}-bit, mean {img.mean():.1f}, max {img.max()}")
