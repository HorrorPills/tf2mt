#!/bin/bash
# Phase-0 P0.4: (a) renderer vsync pacing without msync; (b) the same session restarted under WINEMSYNC=1
# (msync must be enabled for wineserver + Steam + game together, otherwise the game exits silently).
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
DXVK_CONFIG_FILE="$C/async-vsync.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-vsync dxvk +fps_max 0 | tail -14
DXVK_CONFIG_FILE="$C/async-lat1-vsync.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-async-lat1-vsync dxvk +fps_max 0 | tail -14
restart_steam env WINEMSYNC=1
export WINEMSYNC=1
scripts/bench.sh p0t-msync-null-fps0 null +fps_max 0 | tail -14
DXVK_CONFIG_FILE="$C/async.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-msync-async-fps0 dxvk +fps_max 0 | tail -14
DXVK_CONFIG_FILE="$C/async-vsync.conf" DXVK_STATE_CACHE_PATH="$S" scripts/bench.sh p0t-msync-async-vsync dxvk +fps_max 0 | tail -14
unset WINEMSYNC
restart_steam env
