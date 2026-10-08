#!/bin/bash
# Per-preset data collection (mastercomfig Low / Medium / High / Ultra), DXVK vs tf2mt:
#   correctness: demo capture -> replay on both providers -> SSIM of 3 frames (40 %, 70 %, 95 % of the capture)
#   performance: full bench.dem on DXVK and on tf2mt (uncapped, vsync off for tf2mt) + tf2mt ledgers
#   tools/replay/preset-matrix.sh [presets...]     (default: low medium high ultra)
# Output: $TF2_HOME/logs/preset-matrix/<stamp>/summary.md (+ per-preset logs). The owner's mastercomfig preset and
# config.cfg are restored at the end (also on interruption).
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
PRESETS=("$@"); [ ${#PRESETS[@]} -eq 0 ] && PRESETS=(low medium high ultra)
stamp=$(date +%Y%m%d-%H%M)
O=$TF2_HOME/logs/preset-matrix/$stamp; mkdir -p "$O"
orig_preset=$(scripts/mastercomfig.sh status | sed -E 's/.*preset=//')
cp "$TF2DIR/tf/cfg/config.cfg" "$O/config.cfg.orig"
restore() {
  [ "$orig_preset" != none ] && scripts/mastercomfig.sh set "$orig_preset" >/dev/null
  cp "$O/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"
  scripts/layer-uninstall.sh >/dev/null 2>&1
}
trap restore EXIT
log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$O/progress.log"; }

presents() {   # number of PRESENT records in a capture
  python3 - "$1" <<'EOF'
import struct, sys, re
names = re.findall(r'^\s+T9_([A-Z_]+)', open('tools/trace/t9.h').read().split('enum t9_op')[1], re.M)
P = names.index('PRESENT') + 1
f = open(sys.argv[1], 'rb'); f.read(16); n = 0
while True:
    h = f.read(8)
    if len(h) < 8: break
    op, sz = struct.unpack('<II', h)
    if op == P: n += 1
    f.seek(sz, 1)
print(n)
EOF
}

echo "| preset | SSIM (3 frames) | DXVK fps / p99 / 1% low / hitch/min | tf2mt fps / p99 / 1% low / hitch/min | tf2mt CPU ms | tf2mt GPU ms | tf2mt stalls |" > "$O/summary.md"
echo "|---|---|---|---|---|---|---|" >> "$O/summary.md"
for p in "${PRESETS[@]}"; do
  log "=== preset $p"
  scripts/mastercomfig.sh set "$p" | tee -a "$O/progress.log"
  tag=pm-$p-$stamp
  # ---- correctness
  log "capture $tag"
  SECS=25 tools/replay/capture.sh "$tag" demo > "$O/$p-capture.log" 2>&1
  cap=$TF2_HOME/cache/traces/$tag/capture-$tag.t9
  n=$(presents "$cap"); log "capture: $n frames, $(du -h "$cap" | cut -f1)"
  frames="$((n * 40 / 100)),$((n * 70 / 100)),$((n * 95 / 100))"
  DUMP=$frames TIMEOUT=3000 tools/replay/run-replay.sh "$cap" dxvk --frames $((n * 95 / 100 + 1)) > "$O/$p-replay-dxvk.log" 2>&1
  DUMP=$frames TIMEOUT=3000 tools/replay/run-replay.sh "$cap" tf2mt --frames $((n * 95 / 100 + 1)) > "$O/$p-replay-tf2mt.log" 2>&1
  D=$TF2_HOME/cache/traces/$tag
  .venv/bin/python tools/replay/ssim.py "$D/frames-dxvk" "$D/frames-tf2mt" --diff "$D/frames-diff" > "$O/$p-ssim.txt"
  ssim=$(grep -o 'SSIM [0-9.]*' "$O/$p-ssim.txt" | awk '{printf "%s ", $2}')
  log "ssim: $ssim"
  # ---- performance
  log "bench dxvk"
  scripts/bench.sh "$tag-dxvk" dxvk > "$O/$p-bench-dxvk.txt" 2>&1
  log "bench tf2mt"
  TF2MT_VSYNC=0 scripts/bench.sh "$tag-tf2mt" tf2mt > "$O/$p-bench-tf2mt.txt" 2>&1
  b() { awk '/^avg/{fps=$2} /^avg/{for(i=1;i<=NF;i++) if($i=="p99") p99=$(i+1)} /^1% low/{low=$3} /^hitches/{h=$4} END{gsub(/[()\/min]/,"",h); printf "%s / %s / %s / %s", fps, p99, low, h}' "$1"; }
  R=$TF2_HOME/logs/runs/$tag-tf2mt
  cpu=$(grep ledger-perf "$R/unix.log" 2>/dev/null | tail -10 | awk '{s+=$7; n++} END{if(n) printf "%.2f", s/n}')
  gpu=$(grep ledger-perf "$R/unix.log" 2>/dev/null | tail -10 | awk '{s+=$10; n++} END{if(n) printf "%.2f", s/n}')
  stalls=$(grep -c '^stall' "$R/unix.log" 2>/dev/null)
  echo "| $p | $ssim | $(b "$O/$p-bench-dxvk.txt") | $(b "$O/$p-bench-tf2mt.txt") | $cpu | $gpu | $stalls |" | tee -a "$O/summary.md"
done
log "done: $O/summary.md"
