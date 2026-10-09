#!/bin/bash
# demo run with a macOS notification every 15 s (induces the compositor "stuck frame"): night-notif.sh <name> [KEY=VAL ...]
cd "$(dirname "$0")/../.."; . scripts/_env.sh
( for _ in $(seq 90); do tf2_running && break; sleep 1; done; sleep 30; i=0
  while tf2_running; do i=$((i+1)); osascript -e "display notification \"test $i\" with title \"tf2mt latency test\"" >/dev/null 2>&1; sleep 15; done ) &
tools/bench/night-run.sh "$@"; wait
