#!/bin/bash
# Phase-0 P0.4 tuned-DXVK sweep (fps_max 120 = pacing configuration). One fresh process per run.
cd "$(dirname "$0")/.."
C="$HOME/Games/tf2/cache/dxvk"; S="Z:$(echo "$HOME/Games/tf2/cache/dxvk/state" | tr / '\\')"
scripts/bench.sh p0t-null-fps120 null +fps_max 120 | tail -14
DXVK_CONFIG_FILE="$C/async.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-cold-fps120 dxvk +fps_max 120 | tail -14
DXVK_CONFIG_FILE="$C/async.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-warm-fps120 dxvk +fps_max 120 | tail -14
DXVK_CONFIG_FILE="$C/async-lat1.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-lat1-fps120 dxvk +fps_max 120 | tail -14
WINEESYNC=1 DXVK_CONFIG_FILE="$C/async-lat1.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-lat1-esync-fps120 dxvk +fps_max 120 | tail -14
