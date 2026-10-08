#!/bin/bash
# Pacing A/B on the benchmark demo with vsync (120 Hz): input-to-screen latency proxy vs visible jumps.
# Each variant = fresh TF2 process. Results: ~/Games/tf2/logs/lat-<variant>/unix.log ("latency:" lines).
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
caffeinate -d -w $$ &
variants=${VARIANTS:-"base:1:3 ahead0:0:3 draw2:1:2 ahead0draw2:0:2 ahead2:2:3"}
for v in $variants; do
  IFS=: read -r name ahead draw inflight <<<"$v"; inflight=${inflight:-2}
  if [ -x build/tools/displaystate ] && ! build/tools/displaystate >/dev/null 2>&1; then echo "display asleep/locked: stop"; exit 1; fi
  echo "=== $name (game ahead $ahead, drawables $draw) $(date +%H:%M:%S)"
  before=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1)
  TF2_RENDERER=tf2mt TF2MT_VSYNC=1 TF2MT_GAME_AHEAD=$ahead TF2MT_DRAWABLES=$draw TF2MT_INFLIGHT=$inflight DEMO=1 tools/census/tf2mt-smoke.sh "lat-$name" >/dev/null 2>&1
  sess=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1)
  [ "$sess" = "$before" ] && { echo "no new tf2mt session (renderer not used?)"; continue; }
  cp -R "$TF2_HOME/logs/tf2mt/$sess/." "$TF2_HOME/logs/lat-$name/"
  grep -h '^latency:' "$TF2_HOME/logs/lat-$name/unix.log" | tail -1 | cut -c1-200
done
