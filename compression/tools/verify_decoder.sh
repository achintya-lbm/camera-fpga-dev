#!/usr/bin/env bash
# Acceptance check for the reference decoder: decodes every *.jxs / *.jxc in the given directories
# with our jxs_decode and compares sample-exactly against the ISO reference — the conformance image
# (<n>.pgx) when present, otherwise the ISO reference decoder's output.
#
#   compression/tools/verify_decoder.sh compression/testdata/iso21122-4 /tmp/jxs_smoke
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
cd "$root"
bazel build //compression/jxs:jxs_decode @jxs_reference//:jxs_decoder >/dev/null 2>&1 || { echo "build failed"; exit 2; }
bin=$(bazel info bazel-bin 2>/dev/null)
ours="$bin/compression/jxs/jxs_decode"
iso="$bin/external/+camera_fpga_dev_repositories+jxs_reference/jxs_decoder"
tmp=$(mktemp -d)
pass=0; fail=0
for dir in "$@"; do
  for f in "$dir"/*.jxs "$dir"/*.jxc; do
    [[ -f "$f" ]] || continue
    name=$(basename "$f")
    if [[ -f "${f%.*}.pgx_0.h" ]]; then
      reference="${f%.*}.pgx"
    else
      reference="$tmp/${name%.*}_iso.pgm"
      "$iso" -q "$f" "$reference" >/dev/null 2>&1 || { echo "FAIL $name: ISO decoder failed"; fail=$((fail+1)); continue; }
    fi
    if ! "$ours" "$f" "$tmp/${name%.*}_ours.pgm" > "$tmp/${name%.*}.log" 2>&1; then
      echo "FAIL $name: $(tail -n 1 "$tmp/${name%.*}.log")"; fail=$((fail+1)); continue
    fi
    if result=$(python3 "$root/compression/tools/compare_pgm.py" "$reference" "$tmp/${name%.*}_ours.pgm" --codestream "$f" 2>&1 | head -n 1); then
      echo "PASS $name: $result  ($(grep -o 'in [0-9]* ms' "$tmp/${name%.*}.log"))"; pass=$((pass+1))
    else
      echo "FAIL $name: $result"; fail=$((fail+1))
    fi
  done
done
echo "$pass passed, $fail failed"
rm -rf "$tmp"
[[ $fail -eq 0 ]]
