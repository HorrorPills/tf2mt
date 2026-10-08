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
  mkdir -p "$TF2_HOME/run"; touch "$TF2_HOME/run/friends-offline"   # session-end.sh restores it
  sleep 5   # let the status flip settle before the game starts listening
fi

# Renderer (M10): TF2_RENDERER=tf2mt runs the game on the tf2mt Metal renderer instead of DXVK. The layer is installed
# for this session only and removed when TF2 exits (also after a crash), so DXVK is always back afterwards.
# Runs VAC-secured like DXVK. (-insecure can't be used: tf.exe drops it before starting tf_win64.exe.)
renderer=${TF2_RENDERER:-dxvk}
# leftovers from a session that ended without cleanup (e.g. the Mac shut down mid-game) would break the next one
tf2_running || { "$TF2MT_ROOT/scripts/layer-uninstall.sh" >/dev/null 2>&1; rm -f "$TF2DIR/d3d9_ref.dll"; }
# Frame-time recording on DXVK (TF2_FRAMELOG=1): the timing proxy (tools/trace, Phase 0 benchmarks) wraps DXVK for
# this session and is removed on exit. Output: $TF2_HOME/logs/dxvk/<timestamp>/frames-session.csv
if [ "$renderer" = dxvk ] && [ "${TF2_FRAMELOG:-0}" = 1 ] && [ -f "$TF2MT_ROOT/build/trace/d3d9.dll" ]; then
  sess="$TF2_HOME/logs/dxvk/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$sess"
  cp "$TF2MT_ROOT/build/trace/d3d9.dll" "$TF2DIR/d3d9.dll"
  cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2DIR/d3d9_ref.dll"
  export TF2MT_TRACE_DIR="Z:${sess//\//\\}" TF2MT_TAG=session
  "$TF2MT_ROOT/scripts/mastercomfig.sh" status > "$sess/preset.txt" 2>/dev/null
  renderer=dxvk-logged
fi
if [ "$renderer" = tf2mt ]; then
  if "$TF2MT_ROOT/scripts/layer-install.sh" >/dev/null 2>&1; then
    sess="$TF2_HOME/logs/tf2mt/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$sess"
    ls -dt "$TF2_HOME/logs/tf2mt"/*/ 2>/dev/null | tail -n +6 | xargs rm -rf 2>/dev/null   # keep the last 5 sessions
    export TF2MT_UNIX_LOG="$sess/unix.log" TF2MT_TRACE_DIR="Z:${sess//\//\\}" TF2MT_TAG=session
    export TF2MT_FRAME_LOG="Z:${sess//\//\\}\\frames.csv"   # per-frame times (tools/bench/session_report.py)
    "$TF2MT_ROOT/scripts/mastercomfig.sh" status > "$sess/preset.txt" 2>/dev/null
  else
    echo "tf2mt layer not available (build it: make frontend unixlib) — starting with DXVK"; renderer=dxvk
  fi
fi

"$WINE" tf.exe -steam -game tf -novid -nojoy -nosteamcontroller -nohltv -particles 1 "$@" >"$TF2_HOME/logs/tf2.log" 2>&1 &
disown

if [ "$renderer" = dxvk-logged ]; then   # remove the timing proxy when the game is gone
  ( for _ in $(seq 120); do tf2_running && break; sleep 1; done
    while tf2_running; do sleep 5; done
    rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll" ) >/dev/null 2>&1 &
  disown
  echo "renderer: DXVK with frame-time recording; logs: $sess"
fi
if [ "$renderer" = tf2mt ]; then   # remove the layer when the game is gone (normal exit or crash)
  (
    for _ in $(seq 120); do tf2_running && break; sleep 1; done
    while tf2_running; do sleep 5; done
    "$TF2MT_ROOT/scripts/layer-uninstall.sh" >/dev/null 2>&1
  ) >/dev/null 2>&1 &
  disown
  echo "renderer: tf2mt; logs: $sess"
fi

if [ $friends_offline = 1 ]; then   # restore Friends when the game is gone
  (
    for _ in $(seq 120); do tf2_running && break; sleep 1; done
    while tf2_running; do sleep 5; done
    # the launcher's session helper (session-end.sh) restores Friends and shuts Steam down; this is only the
    # fallback for terminal use. Never start Steam just for this (that left a background Steam running).
    sleep 4
    h=$(cat "$TF2_HOME/run/helper.pid" 2>/dev/null)
    if ! { [ -n "$h" ] && kill -0 "$h" 2>/dev/null; } && [ -f "$TF2_HOME/run/friends-offline" ] && steam_running; then
      steam_url steam://friends/status/online; rm -f "$TF2_HOME/run/friends-offline"
    fi
  ) >/dev/null 2>&1 &
  disown
  echo "tf2 started (Friends set offline until TF2 exits; TF2_FRIENDS_ONLINE=1 to keep online) (log: $TF2_HOME/logs/tf2.log)"
else
  echo "tf2 started (log: $TF2_HOME/logs/tf2.log)"
fi
