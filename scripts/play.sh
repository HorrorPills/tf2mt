#!/bin/bash
# One-click launch (used by /Applications/Team Fortress 2.app): Steam if needed (waits for login), then TF2.
. "$(dirname "$0")/_env.sh"
notify() { osascript -e "display notification \"$1\" with title \"Team Fortress 2\"" >/dev/null 2>&1; }
if tf2_running; then notify "TF2 is already running."; exit 0; fi
if ! steam_running; then
  n=$(wc -l < "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null || echo 0)
  notify "Starting Steam…"
  # game-only options (Metal HUD, DXVK config) must not leak into the Steam client session
  env -u MTL_HUD_ENABLED -u DXVK_CONFIG_FILE "$TF2MT_ROOT/scripts/steam.sh" >/dev/null
  # TF2 started before Steam has logged on runs -insecure and crashes during load: wait for a fresh "Logged On"
  for _ in $(seq 120); do
    tail -n +$((n + 1)) "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null | grep -q 'Logged On' && break
    sleep 2
  done
  tail -n +$((n + 1)) "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null | grep -q 'Logged On' \
    || { notify "Steam did not log in within 4 minutes — open the Steam window to check."; exit 1; }
  sleep 5
fi
"$TF2MT_ROOT/scripts/tf2.sh" "$@"
for _ in $(seq 60); do tf2_running && exit 0; sleep 1; done
notify "TF2 did not start — see $TF2_HOME/logs/tf2.log"; exit 1
