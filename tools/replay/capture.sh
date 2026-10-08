#!/bin/bash
# Capture one TF2 scenario as a .t9 call stream (tools/trace capture mode) for tools/replay.
#   tools/replay/capture.sh <tag> <scenario>
# Scenarios (M5 acceptance uses all three):
#   harvest   main menu → koth_harvest_final, 22 bots, first-person spectating
#   loadout   main menu → ctf_2fort, spectating, then the loadout (character) screen
#   mapchange main menu → pl_badwater → changelevel cp_dustbowl (resource churn across a map change)
#   reset     koth_harvest_final, then mat_setvideomode 1280x720 and back (device Reset)
#   demo      tf/bench.dem (koth_harvest_final combat, Phase 0 benchmark) for SECS s of playback, then TF2 is stopped
#             (no MOTD over the world: the M6 pixel gate uses this one)
#             ARGS="+mat_hdr_level 2 ..." adds launch cvars (owner's config.cfg is restored afterwards);
#             EXEC="cmd; wait 600; cmd" runs a temporary tf/cfg/tf2mt_capture.cfg at launch (`wait N` counts frames; deleted after)
# Output: $TF2_HOME/cache/traces/<tag>/capture-<tag>.t9 (+ frames-<tag>.info.txt with the capture summary).
# Same safety rails as the census: -insecure, local listen server, non-local connection guard, config.cfg restored.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
tag=$1; scenario=$2
SECS=${SECS:-30}
R=$TF2_HOME/cache/traces/$tag; rm -rf "$R"; mkdir -p "$R"
rcon() { python3 tools/bench/rcon.py "$@" >/dev/null 2>&1; }
wait_spawn() { for _ in $(seq 150); do tail -n +"$1" "$TF2DIR/tf/console.log" | grep -q 'Client reached server_spawn' && return 0; sleep 2; done; return 1; }
case $scenario in
  harvest) first=koth_harvest_final ;;
  loadout) first=ctf_2fort ;;
  mapchange) first=pl_badwater ;;
  demo) first= ;;
  reset) first=koth_harvest_final ;;
  *) echo "unknown scenario $scenario"; exit 2 ;;
esac

pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
steam_running || { scripts/steam.sh >/dev/null; until grep -q 'Logged On' "$STEAM_DIR/logs/connection_log.txt"; do sleep 2; done; sleep 10; }
make -s trace
cp build/trace/d3d9.dll "$TF2DIR/d3d9.dll"; cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2DIR/d3d9_ref.dll"
cp "$TF2DIR/tf/cfg/config.cfg" "$R/config.cfg.orig"
trap 'rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll"; cp "$R/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"' EXIT
: > "$TF2DIR/tf/console.log"
win() { echo "Z:${1//\//\\}"; }
if [ "$scenario" = demo ]; then
  launch=(${ARGS:-} +playdemo bench)
  if [ -n "${EXEC:-}" ]; then
    launch=(${ARGS:-} +playdemo bench +exec tf2mt_capture)
    echo "${EXEC//;/
}" > "$TF2DIR/tf/cfg/tf2mt_capture.cfg"
    trap 'rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll" "$TF2DIR/tf/cfg/tf2mt_capture.cfg"; cp "$R/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"' EXIT
  fi
else launch=(+sv_lan 1 +rcon_password tf2mt +ip 127.0.0.1 +maxplayers 24 +map "$first"); fi
TF2MT_TRACE_MODE=capture TF2MT_TAG=$tag TF2MT_TRACE_DIR="$(win "$R")" \
  scripts/tf2.sh -insecure -condebug -usercon -windowed -noborder -w 1920 -h 1080 +fps_max 60 "${launch[@]}" >/dev/null
for _ in $(seq 60); do tf2_running && break; sleep 1; done
( while tf2_running; do
    if grep -E 'Connecting to matchmaking server|Connected to [0-9]' "$TF2DIR/tf/console.log" | grep -qvE '(127\.0\.0\.1|localhost|loopback)'; then
      echo "GUARD: non-local server connection — killing TF2"; for p in $(tf2_pids); do kill -9 "$p"; done; exit 0
    fi
    sleep 1
  done ) &
if [ "$scenario" = demo ]; then
  for _ in $(seq 200); do grep -q 'Client reached server_spawn' "$TF2DIR/tf/console.log" && break; sleep 2; done
  sleep "$SECS"
  for p in $(tf2_pids); do kill "$p"; done
  for _ in $(seq 30); do tf2_running || break; sleep 1; done
  cp "$TF2DIR/tf/console.log" "$R/console.log"; ls -la "$R"; exit 0
fi
wait_spawn 1 || { echo "no spawn"; exit 1; }
sleep 8
rcon 'sv_cheats 1' 'mp_autoteambalance 0' 'tf_bot_quota_mode fill' 'tf_bot_quota 22' 'mp_waitingforplayers_cancel 1' 'jointeam spectator'
sleep 10
spectate() { local end=$((SECONDS + $1)); while [ $SECONDS -lt $end ]; do rcon 'spec_mode 4' 'spec_next'; sleep 10; done; }
spectate "$SECS"
case $scenario in
  loadout) rcon 'open_charinfo_direct'; sleep 15 ;;
  reset) rcon 'mat_setvideomode 1280 720 1'; sleep 10; rcon 'mat_setvideomode 1920 1080 1'; sleep 10 ;;
  mapchange)
    n=$(wc -l < "$TF2DIR/tf/console.log" | tr -d " ")
    rcon 'changelevel cp_dustbowl'
    wait_spawn "$n" || echo "cp_dustbowl did not load"
    sleep 10; rcon 'jointeam spectator'; spectate "$SECS" ;;
esac
rcon 'quit'
for _ in $(seq 90); do tf2_running || break; sleep 2; done
cp "$TF2DIR/tf/console.log" "$R/console.log"
grep -h "capture:" "$R"/frames-*.info.txt | tail -3
ls -la "$R"
