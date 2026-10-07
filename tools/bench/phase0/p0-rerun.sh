#!/bin/bash
# Phase-0 reruns with the display awake (the 00:15+ runs were invalid: display asleep).
cd "$(dirname "$0")/.."
. scripts/_env.sh
C="$HOME/Games/tf2/cache/dxvk"; S="Z:$(echo "$HOME/Games/tf2/cache/dxvk/state" | tr / '\\')"
restart_steam() {
  scripts/stop.sh >/dev/null
  local n; n=$(wc -l < "$STEAM_DIR/logs/connection_log.txt")
  "$@" scripts/steam.sh >/dev/null
  until tail -n +$((n + 1)) "$STEAM_DIR/logs/connection_log.txt" | grep -q 'Logged On'; do sleep 2; done
  sleep 10
}
DXVK_CONFIG_FILE="$C/async.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0r-async-fps0 dxvk +fps_max 0 | tail -14
DXVK_CONFIG_FILE="$C/async.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0r-async-fps400 dxvk +fps_max 400 | tail -14
DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0r-cacheonly-fps0 dxvk +fps_max 0 | tail -14
scripts/bench.sh p0r-dxvk-default-fps0 dxvk +fps_max 0 | tail -14
DXVK_CONFIG_FILE="$C/async-vsync.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0r-async-vsync dxvk +fps_max 0 | tail -14
restart_steam env WINEMSYNC=1
export WINEMSYNC=1
scripts/bench.sh p0r-msync-null-fps0 null +fps_max 0 | tail -14
DXVK_CONFIG_FILE="$C/async.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0r-msync-async-fps0 dxvk +fps_max 0 | tail -14
unset WINEMSYNC
restart_steam env
