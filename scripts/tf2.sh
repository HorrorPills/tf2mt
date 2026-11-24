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
    # per-frame start / display / camera timing (docs/mouse-input.md); ~30 MB per hour, so opt-in
    { [ "${TF2MT_LATENCY_LOG:-0}" = 1 ] || [ -f "$TF2_HOME/config/latency-log" ]; } && export TF2MT_LATENCY_CSV="$sess/latency.csv"
    "$TF2MT_ROOT/scripts/mastercomfig.sh" status > "$sess/preset.txt" 2>/dev/null
  else
    echo "tf2mt layer not available (build it: make frontend unixlib) — starting with DXVK"; renderer=dxvk
  fi
fi

# Microphone (launcher setting, TF2_MIC_UID = CoreAudio device UID; empty = the Mac's default input). Wine follows the
# macOS default input unless winecoreaudio.drv's DefaultInput / DefaultVoiceInput name an endpoint
# ("{0.0.1.00000000}.{GUID}", Wine 10 mmdevapi/devenum.c). A capture device's GUID lives under devices\1,<UID>
# (winecoreaudio.drv/mmdevdrv.c get_device_guid): reuse Wine's if it has one, else write a stable one derived from the
# UID. An unplugged device falls back to the Mac default. Only written when the choice changes.
apply_mic() {
  local uid="${TF2_MIC_UID:-}" stamp="$TF2_HOME/config/mic-applied" reg="$TF2_HOME/run/mic.reg"
  local drv='HKEY_CURRENT_USER\Software\Wine\Drivers\winecoreaudio.drv'
  [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$uid" ] && return 0
  mkdir -p "$TF2_HOME/run" "$TF2_HOME/config"
  if [ -z "$uid" ]; then
    printf 'REGEDIT4\n\n[%s]\n"DefaultInput"=-\n"DefaultVoiceInput"=-\n' "$drv" > "$reg"
  else
    local hex b id
    hex=$(grep -F -A3 "[Software\\\\Wine\\\\Drivers\\\\winecoreaudio.drv\\\\devices\\\\1,$uid]" "$WINEPREFIX/user.reg" 2>/dev/null |
          grep -oE '"guid"=hex:[0-9a-f,]{47}' | head -1 | sed 's/.*hex://' | tr -d ,)
    [ ${#hex} = 32 ] || hex=$(printf 'tf2mt-mic:%s' "$uid" | md5 -q)
    b=$(echo "$hex" | sed 's/../& /g')
    set -- $b   # GUID struct bytes: Data1-3 little-endian, Data4 as is
    id=$(printf '{0.0.1.00000000}.{%s%s%s%s-%s%s-%s%s-%s%s-%s%s%s%s%s%s}' $4 $3 $2 $1 $6 $5 $8 $7 $9 ${10} ${11} ${12} ${13} ${14} ${15} ${16} | tr a-f A-F)
    printf 'REGEDIT4\n\n[%s\\devices\\1,%s]\n"guid"=hex:%s\n\n[%s]\n"DefaultInput"="%s"\n"DefaultVoiceInput"="%s"\n' \
      "$drv" "$uid" "$(echo "$hex" | sed 's/../&,/g; s/,$//')" "$drv" "$id" "$id" > "$reg"
  fi
  "$WINE" regedit /S "Z:${reg//\//\\}" >/dev/null 2>&1 && printf '%s' "$uid" > "$stamp"
  rm -f "$reg"
}
[ "${TF2_TUNED:-1}" = 1 ] && apply_mic

# Relative mouse mode (issue #2, src/unixlib/relmouse.m): while aiming, the macOS cursor stays parked, because every
# cursor move makes macOS composite the screen and caps the game at the display refresh rate (60 fps on 60 Hz Macs).
# Loaded into the game for both renderers (the Metal renderer also carries it in tf2mt.so). Opt out: TF2_RELMOUSE=0.
# Exported here, not by the caller: macOS drops DYLD_* variables when it starts /bin/bash. The copy in $TF2_HOME/run
# has no quarantine flag (a downloaded app passes it on, and macOS may then refuse to load the library).
if [ "${TF2_RELMOUSE:-1}" != 0 ] && [ -f "$TF2MT_ROOT/build/tf2mt/relmouse.dylib" ]; then
  mkdir -p "$TF2_HOME/run"
  rm_lib="$TF2_HOME/run/relmouse.dylib"
  cmp -s "$TF2MT_ROOT/build/tf2mt/relmouse.dylib" "$rm_lib" || cp "$TF2MT_ROOT/build/tf2mt/relmouse.dylib" "$rm_lib"
  xattr -d com.apple.quarantine "$rm_lib" 2>/dev/null || true
  export DYLD_INSERT_LIBRARIES="$rm_lib${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}"
  export TF2MT_RELMOUSE_LOG="$TF2_HOME/logs/relmouse.log"
elif [ "${TF2_RELMOUSE:-1}" = 0 ]; then
  export TF2MT_RELMOUSE=0   # also keeps the copy in tf2mt.so off
fi
: > "$TF2_HOME/logs/relmouse.log"   # this session's log only (empty when the mode is off)

# TF2 update of 2026-11-23 (build 26876676): Valve removed the DirectX 8 meshes (.dx80.vtx) from tf2_misc_dir.vpk
# (14,400 -> 10). DX levels below 90 now draw DX9 meshes with DX8 shader settings. tf2mt never sets a DX8 level, but a
# player's own launch options or the saved level (registry DXLevel_V1, kept from an old -dxlevel 8x) can. Both are
# moved to 95 (the level tf2mt is built and tested for, docs/decisions/ADR-001). One -dxlevel launch saves the level,
# so the registry fix happens once. Log: $TF2_HOME/logs/migrations.log
migrate_log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$TF2_HOME/logs/migrations.log"; echo "tf2mt: $*"; }
args=(); has_dx=0
while [ $# -gt 0 ]; do
  case "$1" in
    -dxlevel|+mat_dxlevel)
      has_dx=1
      if [[ "${2:-}" =~ ^[0-9]+$ ]] && [ "$2" -lt 90 ]; then
        migrate_log "launch option '$1 $2' changed to '-dxlevel 95' (DX8 meshes were removed from TF2 on 2026-11-23)"
        args+=(-dxlevel 95); shift 2; continue
      fi ;;
  esac
  args+=("$1"); shift
done
if [ "$has_dx" = 0 ]; then
  dxl=$(awk '/^\[Software\\\\Valve\\\\Source\\\\tf\\\\Settings\]/ {s=1; next} /^\[/ {s=0} s && /^"DXLevel_V1"=dword:/ {sub(/.*dword:/, ""); print; exit}' "$WINEPREFIX/user.reg" 2>/dev/null)
  if [ -n "$dxl" ] && [ $((16#$dxl)) -lt 90 ]; then
    migrate_log "saved DirectX level $((16#$dxl)) changed to 95 for this launch (TF2 saves it; video settings are reset to the DX9 defaults)"
    args+=(-dxlevel 95)
  fi
fi
# Experimental Rubikon physics (launcher Debug tab, TF2_RUBIKON=1): Valve's Source 2 physics backport (2026-11-23,
# bin/x64/vphysics_rubikon.dll, 64-bit only). It loads under Wine 10 with no DLL override (imports: tier0, kernel32,
# rpcrt4) and adds the vrn* cvars; tested 2026-11-24 on both renderers (listen server, physics props, ragdoll).
if [ "${TF2_RUBIKON:-0}" = 1 ] && [[ " ${args[*]:-} " != *" -rubikon "* ]]; then args+=(-rubikon); fi
set -- ${args[@]+"${args[@]}"}

# Steam overlay (launcher setting, TF2_STEAM_OVERLAY=1, DXVK only): Steam only injects its overlay into games it starts
# itself, and the in-game store needs the overlay to confirm purchases. The game then inherits Steam's environment
# instead of this script's (DXVK pacing config and frame-time recording don't apply). Not on the Metal renderer: the
# overlay only detects a d3d9.dll in system32, and hooked into tf2mt it blanked the menu background (2026-10-12).
if [ "${TF2_STEAM_OVERLAY:-0}" = 1 ] && [ "$renderer" = dxvk ]; then
  ( cd "$STEAM_DIR" && "$WINE" steam.exe -applaunch 440 -novid -nojoy -nosteamcontroller -nohltv -particles 1 "$@" ) >"$TF2_HOME/logs/tf2.log" 2>&1 &
else
  "$WINE" tf.exe -steam -game tf -novid -nojoy -nosteamcontroller -nohltv -particles 1 "$@" >"$TF2_HOME/logs/tf2.log" 2>&1 &
fi
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
