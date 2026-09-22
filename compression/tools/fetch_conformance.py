#!/usr/bin/env python3
"""Fetch selected members of the ISO/IEC 21122-4 conformance archive (tests.zip, 1.85 GB) with HTTP
range requests, without downloading the whole file. Streams are kept OUT of git (ISO licence).

    compression/tools/fetch_conformance.py --list [--grep bayer]
    compression/tools/fetch_conformance.py --get 210 211 --out testdata/iso21122-4   # by stream number prefix
    compression/tools/fetch_conformance.py --get-name path/in/zip --out DIR
"""
import argparse, os, struct, sys, urllib.request, zlib

URL = "https://standards.iso.org/iso-iec/21122/-4/ed-3/en/tests.zip"

def fetch(start, end):
    req = urllib.request.Request(URL, headers={"Range": f"bytes={start}-{end}"})
    with urllib.request.urlopen(req, timeout=60) as r:
        if r.status not in (200, 206): sys.exit(f"HTTP {r.status}")
        return r.read()

def total_size():
    req = urllib.request.Request(URL, method="HEAD")
    with urllib.request.urlopen(req, timeout=60) as r:
        return int(r.headers["Content-Length"])

def central_directory():
    size = total_size()
    tail = fetch(max(0, size - 65536 - 22), size - 1)
    eocd = tail.rfind(b"PK\x05\x06")
    if eocd < 0: sys.exit("EOCD not found")
    (_, _, _, _, n_entries, cd_size, cd_offset, _) = struct.unpack("<IHHHHIIH", tail[eocd:eocd + 22])
    if n_entries == 0xFFFF or cd_offset == 0xFFFFFFFF:  # zip64
        loc = tail.rfind(b"PK\x06\x07")
        z64_off = struct.unpack("<Q", tail[loc + 8:loc + 16])[0]
        z64 = fetch(z64_off, z64_off + 56 - 1)
        n_entries = struct.unpack("<Q", z64[32:40])[0]
        cd_size = struct.unpack("<Q", z64[40:48])[0]
        cd_offset = struct.unpack("<Q", z64[48:56])[0]
    cd = fetch(cd_offset, cd_offset + cd_size - 1)
    entries = []; pos = 0
    for _ in range(n_entries):
        if cd[pos:pos + 4] != b"PK\x01\x02": sys.exit("bad central directory")
        (_, _, _, _, method, _, _, crc, csize, usize, nlen, elen, clen, _, _, _, lho) = struct.unpack("<IHHHHHHIIIHHHHHII", cd[pos:pos + 46])
        name = cd[pos + 46:pos + 46 + nlen].decode("utf-8", "replace")
        extra = cd[pos + 46 + nlen:pos + 46 + nlen + elen]
        if usize == 0xFFFFFFFF or csize == 0xFFFFFFFF or lho == 0xFFFFFFFF:  # zip64 extra field
            e = 0
            while e + 4 <= len(extra):
                tag, ln = struct.unpack("<HH", extra[e:e + 4]); body = extra[e + 4:e + 4 + ln]
                if tag == 1:
                    vals = list(struct.unpack("<" + "Q" * (ln // 8), body[:ln // 8 * 8])); i = 0
                    if usize == 0xFFFFFFFF: usize = vals[i]; i += 1
                    if csize == 0xFFFFFFFF: csize = vals[i]; i += 1
                    if lho == 0xFFFFFFFF: lho = vals[i]; i += 1
                e += 4 + ln
        entries.append(dict(name=name, method=method, csize=csize, usize=usize, offset=lho, crc=crc))
        pos += 46 + nlen + elen + clen
    return entries

def extract(entry, out_dir):
    head = fetch(entry["offset"], entry["offset"] + 30 - 1)
    if head[:4] != b"PK\x03\x04": sys.exit("bad local header")
    nlen, elen = struct.unpack("<HH", head[26:30])
    start = entry["offset"] + 30 + nlen + elen
    data = fetch(start, start + entry["csize"] - 1) if entry["csize"] else b""
    if entry["method"] == 8: data = zlib.decompress(data, -15)
    elif entry["method"] != 0: sys.exit(f"unsupported compression method {entry['method']}")
    if zlib.crc32(data) & 0xFFFFFFFF != entry["crc"]: sys.exit(f"CRC mismatch for {entry['name']}")
    path = os.path.join(out_dir, entry["name"])
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f: f.write(data)
    print(f"{path}: {len(data)} bytes")

ap = argparse.ArgumentParser()
ap.add_argument("--list", action="store_true")
ap.add_argument("--grep", help="substring filter for --list (case-insensitive)")
ap.add_argument("--get", nargs="*", help="stream-number prefixes to fetch (all members whose basename starts with the number)")
ap.add_argument("--get-name", nargs="*", help="exact member names to fetch")
ap.add_argument("--out", default="testdata/iso21122-4")
args = ap.parse_args()
entries = central_directory()
if args.list:
    for e in entries:
        if not e["name"].endswith("/") and (not args.grep or args.grep.lower() in e["name"].lower()):
            print(f"{e['usize']:>12}  {e['name']}")
    print(f"{len(entries)} members", file=sys.stderr)
for prefix in args.get or []:
    hits = [e for e in entries if not e["name"].endswith("/") and os.path.basename(e["name"]).startswith(prefix)]
    if not hits: print(f"no member starts with {prefix}", file=sys.stderr)
    for e in hits: extract(e, args.out)
for name in args.get_name or []:
    hits = [e for e in entries if e["name"] == name]
    if not hits: sys.exit(f"no member {name}")
    extract(hits[0], args.out)
