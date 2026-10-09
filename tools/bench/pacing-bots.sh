#!/bin/bash
# Pacing variants under game-frame spikes: Medium preset bot match at 120 Hz vsync. name:ahead:drawables
cd "$(dirname "$0")/../.."; . scripts/_env.sh
orig=$(scripts/mastercomfig.sh status | sed -E 's/.*preset=([a-z]+).*/\1/')
trap 'scripts/mastercomfig.sh set "$orig" >/dev/null 2>&1; echo "preset restored: $orig"' EXIT
scripts/mastercomfig.sh set "${PRESET:-medium}" >/dev/null 2>&1
variants=${VARIANTS:-"pace-a0d2:0:2 pace-a1d2:1:2 pace-a0d3:0:3 pace-a1d3:1:3"}
for v in $variants; do
  IFS=: read -r n a dr <<<"$v"
  SECS=${SECS:-200} tools/bench/night-bots.sh "$n" VSYNC=1 WARP=1 TF2MT_GAME_AHEAD=$a TF2MT_DRAWABLES=$dr 2>&1 | grep -v Terminated
done
