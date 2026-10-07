#!/bin/bash
# Phase-0 P0.4 follow-up: Wine msync (mach semaphores) with the async DXVK config.
cd "$(dirname "$0")/.."
while pgrep -f p0-tuning.sh >/dev/null; do sleep 5; done
C="$HOME/Games/tf2/cache/dxvk"; S="Z:$(echo "$HOME/Games/tf2/cache/dxvk/state" | tr / '\\')"
WINEMSYNC=1 DXVK_CONFIG_FILE="$C/async-lat1.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-lat1-msync-fps120 dxvk +fps_max 120 | tail -14
WINEMSYNC=1 DXVK_CONFIG_FILE="$C/async-lat1.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-lat1-msync-fps0 dxvk +fps_max 0 | tail -14
WINEMSYNC=1 scripts/bench.sh p0t-null-msync-fps0 null +fps_max 0 | tail -14
