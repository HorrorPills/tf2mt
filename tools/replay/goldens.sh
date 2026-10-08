#!/bin/bash
# Run the golden frame set (tests/golden/frames.tsv): dump each listed frame on DXVK (cached per capture) and on
# tf2mt, compare with tools/replay/ssim.py. Exit 1 if any frame is below MIN_SSIM (default 0.995).
#   tools/replay/goldens.sh [tag ...]        (default: every row)   FRESH=1 re-dumps the DXVK reference.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
MIN=${MIN_SSIM:-0.995}
T=$TF2_HOME/cache/traces
fail=0
while IFS=$'\t' read -r tag frames; do
  case "$tag" in ''|\#*) continue ;; esac
  [ $# -gt 0 ] && ! printf '%s\n' "$@" | grep -qx "$tag" && continue
  c=$T/$tag/capture-$tag.t9
  [ -f "$c" ] || { echo "$tag: capture missing"; fail=1; continue; }
  last=$(echo "$frames" | tr , '\n' | sort -n | tail -1)
  want=$(echo "$frames" | tr , '\n' | sed 's/.*/frame-&/' | awk '{printf "frame-%05d.ppm\n", substr($0,7)}')
  have=$(ls "$T/$tag/frames-dxvk" 2>/dev/null)
  if [ -n "${FRESH:-}" ] || [ "$(echo "$want" | sort)" != "$(echo "$have" | sort)" ]; then
    DUMP=$frames TIMEOUT=2400 tools/replay/run-replay.sh "$c" dxvk --frames $((last + 1)) >/dev/null 2>&1
  fi
  DUMP=$frames TIMEOUT=2400 tools/replay/run-replay.sh "$c" tf2mt --frames $((last + 1)) >/dev/null 2>&1
  out=$(.venv/bin/python tools/replay/ssim.py "$T/$tag/frames-dxvk" "$T/$tag/frames-tf2mt" --diff "$T/$tag/frames-diff")
  echo "$out" | sed "s/^/$tag /" | grep -v "^$tag min"
  m=$(echo "$out" | awk '/min SSIM/{print $3}')
  awk -v m="$m" -v t="$MIN" 'BEGIN{exit !(m < t)}' && { echo "$tag: BELOW $MIN (min $m)"; fail=1; }
done < tests/golden/frames.tsv
echo "goldens: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
