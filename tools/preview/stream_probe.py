#!/usr/bin/env python3
"""Probe a cam_tuner MJPEG stream: counts parts, all-black frames, decode errors and part intervals.

    tools/preview/stream_probe.py http://<host>:8080/stream.mjpg [seconds] [--save-dir DIR]

Used to diagnose the 2026-09-21 flicker (alternating all-black JPEGs from an unsynchronised CUDA
stream). Needs Pillow + numpy for the brightness statistics; without them only sizes/timing print.
"""
import argparse, io, sys, time, urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("url")
ap.add_argument("seconds", type=float, nargs="?", default=4.0)
ap.add_argument("--save-dir", help="write the first three decoded frames here (grey PNGs)")
ap.add_argument("--quiet", action="store_true", help="only print the summary lines")
args = ap.parse_args()

req = urllib.request.urlopen(args.url, timeout=10)
if not args.quiet:
    print("content-type:", req.headers.get("Content-Type"))
buf = bytearray(); marks = []
t0 = time.time()
while time.time() - t0 < args.seconds:
    chunk = req.read(65536)
    if not chunk:
        break
    buf += chunk; marks.append((len(buf), time.time() - t0))

frames = []; pos = 0
while True:
    s = buf.find(b"\xff\xd8", pos)
    if s < 0: break
    e = buf.find(b"\xff\xd9", s)
    if e < 0: break
    frames.append((s, e + 2)); pos = e + 2

def t_at(off):
    for o, t in marks:
        if o >= off: return t
    return marks[-1][1] if marks else 0.0

try:
    from PIL import Image
    import numpy as np
    have_pil = True
except Exception as ex:  # pragma: no cover
    have_pil = False
    print("no PIL/numpy:", ex, file=sys.stderr)

black = bad = 0; times = []; means = []
for i, (s, e) in enumerate(frames):
    data = bytes(buf[s:e]); t = t_at(e); times.append(t)
    line = f"frame {i:3d} t={t:6.3f}s size={len(data):7d}"
    if have_pil:
        try:
            im = Image.open(io.BytesIO(data)).convert("L")
            a = np.asarray(im, dtype=np.float32)
            mean = float(a.mean()); means.append(mean)
            if mean < 0.5: black += 1
            line += f" mean={mean:6.2f} p99={float(np.percentile(a, 99)):6.1f}"
            if args.save_dir and i < 3:
                im.save(f"{args.save_dir}/stream_frame_{i}.png")
        except Exception as ex:
            bad += 1; line += f" DECODE ERROR {ex}"
    if not args.quiet:
        print(line)
d = [b - a for a, b in zip(times, times[1:])]
print(f"parts={len(frames)} bytes={len(buf)} black={black} decode_errors={bad}"
      + (f" mean_min={min(means):.1f} mean_max={max(means):.1f}" if means else ""))
if d:
    print(f"intervals: min={min(d)*1000:.0f} ms max={max(d)*1000:.0f} ms mean={sum(d)/len(d)*1000:.0f} ms")
sys.exit(1 if (black or bad or not frames) else 0)
