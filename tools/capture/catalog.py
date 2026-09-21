#!/usr/bin/env python3
"""Build a markdown table from capture_modes.sh output directories.

Reads <out>/<mode>/summary.json (bandwidth_test), the newest <camera>_NNN_fNNNNNN.json sidecar
(lane rate, HMAX, VMAX, geometry) and decode.json (raw_frame statistics), and prints one row per
mode with image links relative to <out>.
"""
import json
import pathlib
import sys


def parse_concatenated_json(text: str) -> list:
    objs, depth, start = [], 0, None
    for i, ch in enumerate(text):
        if ch == "{":
            if depth == 0:
                start = i
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0 and start is not None:
                objs.append(json.loads(text[start : i + 1]))
    return objs


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "captures/imx676_cam4")
    rows = []
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        summary = d / "summary.json"
        if not summary.exists():
            continue
        s = json.loads(summary.read_text())
        cam = s["cameras"][0] if s.get("cameras") else {}
        sidecars = sorted(p for p in d.glob("*_f*.json") if not p.name.startswith("decode"))
        side = json.loads(sidecars[-1].read_text()) if sidecars else {}
        stats, preview, crop = {}, "", ""
        decode = d / "decode.json"
        if decode.exists():
            objs = parse_concatenated_json(decode.read_text())
            if objs:
                stats = objs[-1].get("stats", {})
                outs = objs[-1].get("outputs", {})
                preview = outs.get("preview", "")
                crop = outs.get("center_crop", "")
        rows.append((d.name, cam, side, stats, preview, crop))

    print("| Mode | Output | Lane rate | HMAX / VMAX | Expected fps | Measured fps | Gbps (CSI) | Frames | Gaps | Drops | CRC bad/checked | p1 / p99 / saturated | Preview | 1:1 centre |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name, cam, side, st, preview, crop in rows:
        pct = f"{st.get('p01', '')} / {st.get('p99', '')} / {st.get('saturated_fraction', 0):.4f}" if st else ""
        rel = lambda p: f"{name}/{pathlib.Path(p).name}" if p else ""
        img = f"![{name}]({rel(preview)})" if preview else ""
        crop_link = f"[png]({rel(crop)})" if crop else ""
        out = f"{side.get('width', '')}×{side.get('height', '')} {side.get('pixel_format', '')}" if side else ""
        rate = f"{side.get('lane_rate_mbps', '')} Mbps" if side else ""
        hv = f"{side.get('hmax', '')} / {side.get('vmax', '')}" if side else ""
        print(f"| `{name}` | {out} | {rate} | {hv} | {cam.get('expected_fps', 0):.2f} | {cam.get('measured_fps', 0):.2f} | "
              f"{cam.get('measured_gbps', 0):.3f} | {cam.get('frames', 0)} | {cam.get('gaps', 0)} | {cam.get('dropped', 0)} | "
              f"{cam.get('crc_bad', 0)}/{cam.get('crc_checked', 0)} | {pct} | {img} | {crop_link} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
