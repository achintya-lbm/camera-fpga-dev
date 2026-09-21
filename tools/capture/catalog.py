#!/usr/bin/env python3
"""Build a markdown table from capture_modes.sh output directories (summary.json + decode.json)."""
import json
import pathlib
import sys


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "captures/imx676_cam4")
    rows = []
    for d in sorted(p for p in root.iterdir() if p.is_dir()):
        summary = d / "summary.json"
        if not summary.exists():
            continue
        s = json.loads(summary.read_text())
        cam = s["cameras"][0] if s.get("cameras") else {}
        stats = {}
        decode = d / "decode.json"
        if decode.exists():
            text = decode.read_text().strip()
            # raw_frame prints one JSON object per frame, concatenated
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
            if objs:
                stats = objs[-1].get("stats", {})
                preview = objs[-1].get("outputs", {}).get("preview", "")
            else:
                preview = ""
        else:
            preview = ""
        rows.append((d.name, cam, stats, preview))
    print("| Mode | Lane rate | Expected fps | Measured fps | Gbps (CSI) | Frames | Gaps | Drops | CRC bad | p1 / p99 / saturated | Preview |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for name, cam, st, preview in rows:
        pct = f"{st.get('p01', '')} / {st.get('p99', '')} / {st.get('saturated_fraction', 0):.4f}" if st else ""
        prev = f"![{name}]({pathlib.Path(preview).name})" if preview else ""
        print(f"| `{name}` | | {cam.get('expected_fps', 0):.2f} | {cam.get('measured_fps', 0):.2f} | {cam.get('measured_gbps', 0):.3f} | "
              f"{cam.get('frames', 0)} | {cam.get('gaps', 0)} | {cam.get('dropped', 0)} | {cam.get('crc_bad', 0)}/{cam.get('crc_checked', 0)} | {pct} | {prev} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())
