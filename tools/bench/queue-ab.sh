#!/bin/bash
# mat_queue_mode A/B on the benchmark demo (Metal renderer, uncapped): fps, frame-time spikes, per-thread CPU.
# Results: ~/Games/tf2/logs/qm-<variant>/ (session logs + threads.txt). Variants: "name:mode" list in VARIANTS.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
caffeinate -d -w $$ &
variants=${VARIANTS:-"auto:-1 q2:2 q0:0"}
for v in $variants; do
  IFS=: read -r name mode <<<"$v"
  state=$(build/tools/displaystate) || { echo "display asleep/locked: stop ($state)"; exit 1; }
  echo "=== $name (mat_queue_mode $mode) $(date +%H:%M:%S)"
  out="$TF2_HOME/logs/qm-$name"; rm -rf "$out"; mkdir -p "$out"
  before=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1)
  ( # per-thread CPU of tf_win64 every 5 s while it runs: top 4 threads
    for _ in $(seq 60); do tf2_running && break; sleep 1; done
    while tf2_running; do
      p=$(tf2_pids | head -1); [ -n "$p" ] && ps -M -p "$p" 2>/dev/null | awk 'NR>1 {for(i=1;i<=NF;i++) if ($i ~ /^[0-9.]+$/ && $(i+1) ~ /^[RSUTIZ]/) {print $i; break}}' | sort -rn | head -4 | tr '\n' ' ' >> "$out/threads.txt" && echo >> "$out/threads.txt"
      sleep 5
    done ) &
  watcher=$!
  TF2_RENDERER=tf2mt TF2MT_VSYNC=0 DEMO=1 tools/census/tf2mt-smoke.sh "qm-$name-smoke" +mat_queue_mode "$mode" >/dev/null 2>&1
  wait $watcher 2>/dev/null
  sess=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1)
  [ "$sess" = "$before" ] && { echo "no new session"; continue; }
  cp -R "$TF2_HOME/logs/tf2mt/$sess/." "$out/"
done
