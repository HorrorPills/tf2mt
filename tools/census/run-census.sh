#!/bin/bash
# Census run (PLAN.md P1): one TF2 session behind the trace proxy in census mode, driven over RCON.
#   tools/census/run-census.sh <tag> [extra launch args…]       e.g.  run-census.sh msaa4 +mat_antialias 4
# Sequence: koth_harvest_final with 22 bots → first-person spectating (spec_next every 15 s) → loadout screen →
# screenshot/jpeg → changelevel through $MAPS (75 s each) → quit. Output in $TF2_HOME/logs/census/<tag>/,
# shaders into ~/Code/tf2mt/corpus (shared, content-addressed).
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
tag=$1; shift
MAPS=${MAPS:-"ctf_2fort pl_badwater cp_dustbowl plr_hightower arena_well pd_watergate cp_process_final"}
SECS=${SECS:-75}
R=$TF2_HOME/logs/census/$tag; rm -rf "$R"; mkdir -p "$R" corpus
rcon() { python3 tools/bench/rcon.py "$@" >/dev/null 2>&1; }
wait_spawn() {  # wait for a new "Client reached server_spawn" after line $1
  for _ in $(seq 150); do tail -n +"$1" "$TF2DIR/tf/console.log" | grep -q 'Client reached server_spawn' && return 0; sleep 2; done; return 1; }

pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
ensure_steam || { echo "Steam did not log in"; exit 1; }
cp build/trace/d3d9.dll "$TF2DIR/d3d9.dll"; cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2DIR/d3d9_ref.dll"
# variants change cvars that TF2 saves on exit: keep the owner's config untouched
cp "$TF2DIR/tf/cfg/config.cfg" "$R/config.cfg.orig"
trap 'rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll"; cp "$R/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"' EXIT
: > "$TF2DIR/tf/console.log"
win() { echo "Z:${1//\//\\}"; }
TF2_FRIENDS_ONLINE=1 TF2MT_TRACE_MODE=census TF2MT_TAG=$tag TF2MT_TRACE_DIR="$(win "$R")" TF2MT_CORPUS_DIR="$(win "$PWD/corpus")" \
  scripts/tf2.sh -insecure -condebug -usercon -windowed -noborder -w 1920 -h 1080 +sv_lan 1 +rcon_password tf2mt +ip 127.0.0.1 \
  +maxplayers 24 "$@" +map koth_harvest_final >/dev/null
for _ in $(seq 60); do tf2_running && break; sleep 1; done
# safety guard (second layer after -insecure): test builds must never stay on a non-local server
( while tf2_running; do
    if grep -E 'Connecting to matchmaking server|Connected to [0-9]' "$TF2DIR/tf/console.log" | grep -qvE '(127\.0\.0\.1|localhost|loopback)'; then
      echo "GUARD: non-local server connection — killing TF2"; for p in $(tf2_pids); do kill -9 "$p"; done; exit 0
    fi
    sleep 1
  done ) &
wait_spawn 1 || { echo "no spawn"; exit 1; }
sleep 8
rcon 'sv_cheats 1' 'mp_autoteambalance 0' 'tf_bot_quota_mode fill' 'tf_bot_quota 22' 'mp_waitingforplayers_cancel 1' 'jointeam spectator'
sleep 20
spectate() { local end=$((SECONDS + $1)); while [ $SECONDS -lt $end ]; do rcon 'spec_mode 4' 'spec_next'; sleep 15; done; }
spectate "$SECS"
rcon 'open_charinfo_direct'; sleep 15; rcon 'screenshot'; sleep 2; rcon 'jpeg'; sleep 3
for m in $MAPS; do
  n=$(wc -l < "$TF2DIR/tf/console.log" | tr -d " ")
  rcon "changelevel $m"
  wait_spawn "$n" || { echo "map $m did not load"; continue; }
  sleep 10; rcon 'jointeam spectator'; spectate "$SECS"
done
rcon 'quit'
for _ in $(seq 60); do tf2_running || break; sleep 2; done
cp "$TF2DIR/tf/console.log" "$R/console.log"
ls "$R"; echo "corpus: $(ls corpus | wc -l) shaders"
