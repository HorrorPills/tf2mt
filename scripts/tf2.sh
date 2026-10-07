#!/bin/bash
# launch TF2 (64-bit) via its stub; extra args are passed through
. "$(dirname "$0")/_env.sh"; . "$(dirname "$0")/_tuned.sh"; cd "$TF2DIR" || exit 1
mkdir -p "$TF2_HOME/logs"
follow_session_sync

# Steam Friends offline while TF2 runs (docs/loadout-stall.md): TF2's main-menu friends panel re-queries
# all friends over Steam IPC on every friend status change; under Wine that is ~1000 x 1 ms calls per frame
# -> 2-4 fps in menus/loadout. Offline stops the updates. Restored to online when TF2 exits.
# Opt out: TF2_FRIENDS_ONLINE=1. Skipped for benchmark runs (TF2_TUNED=0).
steam_url() { (cd "$STEAM_DIR" && "$WINE" steam.exe "$1" >/dev/null 2>&1); }
friends_offline=0
if [ "${TF2_FRIENDS_ONLINE:-0}" != 1 ] && [ "${TF2_TUNED:-1}" = 1 ] && pgrep -f steamwebhelper >/dev/null; then
  steam_url steam://friends/status/offline; friends_offline=1
  sleep 5   # let the status flip settle before the game starts listening
fi

"$WINE" tf.exe -steam -game tf -novid -nojoy -nosteamcontroller -nohltv -particles 1 "$@" >"$TF2_HOME/logs/tf2.log" 2>&1 &
disown

if [ $friends_offline = 1 ]; then   # restore Friends when the game is gone
  (
    for _ in $(seq 120); do tf2_running && break; sleep 1; done
    while tf2_running; do sleep 5; done
    steam_url steam://friends/status/online
  ) >/dev/null 2>&1 &
  disown
  echo "tf2 started (Friends set offline until TF2 exits; TF2_FRIENDS_ONLINE=1 to keep online) (log: $TF2_HOME/logs/tf2.log)"
else
  echo "tf2 started (log: $TF2_HOME/logs/tf2.log)"
fi
