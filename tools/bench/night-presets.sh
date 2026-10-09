#!/bin/bash
# Overnight preset matrix: bot match on each mastercomfig preset, vsync and uncapped. Restores the owner's preset.
cd "$(dirname "$0")/../.."; . scripts/_env.sh
orig=$(scripts/mastercomfig.sh status | sed -E 's/.*preset=([a-z]+).*/\1/')
trap 'scripts/mastercomfig.sh set "$orig" >/dev/null 2>&1; echo "preset restored: $orig"' EXIT
for p in ${PRESETS:-low medium high ultra}; do
  scripts/mastercomfig.sh set "$p" >/dev/null 2>&1 || { echo "could not set $p"; continue; }
  for v in ${MODES:-1 0}; do
    SECS=${SECS:-180} tools/bench/night-bots.sh "P-$p-$([ $v = 1 ] && echo vsync || echo uncapped)" VSYNC=$v WARP=1 2>&1 | grep -v Terminated
  done
done
