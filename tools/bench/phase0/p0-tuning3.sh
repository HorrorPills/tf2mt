#!/bin/bash
# Phase-0 P0.4: renderer-side vsync pacing (fps_max 0, DXVK presentInterval 1 on the 120 Hz panel).
cd "$(dirname "$0")/.."
while pgrep -f 'p0-tuning2?\.sh' >/dev/null; do sleep 5; done
C="$HOME/Games/tf2/cache/dxvk"; S="Z:$(echo "$HOME/Games/tf2/cache/dxvk/state" | tr / '\\')"
WINEMSYNC=1 DXVK_CONFIG_FILE="$C/async-lat1-vsync.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-lat1-msync-vsync dxvk +fps_max 0 | tail -14
WINEMSYNC=1 DXVK_CONFIG_FILE="$C/async-vsync.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-msync-vsync dxvk +fps_max 0 | tail -14
