#!/bin/bash
# One benchmark-demo run on the Metal renderer for the overnight study. usage: night-run.sh <name> [KEY=VAL ...]
# Extra env: WARP=1 (cursor recentring at 120 Hz like TF2 aiming), WARP=events (plus synthetic mouse moves),
# VSYNC=0|1 (default 1). Output: ~/Games/tf2/logs/night/<name>/{latency.csv,unix.log,frames.csv,console.log,...}
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
name=$1; shift
for kv in "$@"; do export "$kv"; done
out="$TF2_HOME/logs/night/$name"; rm -rf "$out"; mkdir -p "$out"
state=$(build/tools/displaystate) || { echo "$name: display asleep/locked ($state)"; exit 1; }
tf2_running && { echo "$name: TF2 already running"; exit 1; }
before=$(ls -t "$TF2_HOME/logs/tf2mt" 2>/dev/null | head -1)
if [ "${WARP:-0}" != 0 ]; then
  ( for _ in $(seq 90); do tf2_running && break; sleep 1; done; sleep 20
    while tf2_running; do build/tools/cursorwarp 120 30 $([ "$WARP" = events ] && echo events); done ) &
fi
TF2_RENDERER=tf2mt TF2MT_VSYNC=${VSYNC:-1} TF2MT_LATENCY_CSV="$out/latency.csv" DEMO=1 \
  tools/census/tf2mt-smoke.sh "night-$name" >/dev/null 2>&1
wait
sess=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1)
[ "$sess" != "$before" ] && cp -R "$TF2_HOME/logs/tf2mt/$sess/." "$out/"
cp "$TF2_HOME/logs/night-$name/console.log" "$out/console.log" 2>/dev/null
echo "$name: done $(date +%H:%M:%S)"
