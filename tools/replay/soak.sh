#!/bin/bash
# M7 soak: TF2 live on tf2mt (no DXVK), local listen server, N map changes (default 50) cycling a map list.
# After each load: the tf2mt upload ledger's live counts (buffers/textures and MB) are sampled -> leak trend.
#   tools/replay/soak.sh [N]        output: $TF2_HOME/logs/soak/{soak.tsv,unix.log,tf2mt-soak.log,console.log}
# Same safety rails as captures: -insecure, local server, non-local guard, config.cfg restored, layer uninstalled.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
N=${1:-50}
PROVIDER=${PROVIDER:-tf2mt}   # PROVIDER=dxvk: same run on the stock DXVK setup (baseline for memory growth)
MAPS=(koth_harvest_final ctf_2fort pl_badwater cp_dustbowl cp_process_final pl_upward)
L=$TF2_HOME/logs/soak${PROVIDER/tf2mt/}; rm -rf "$L"; mkdir -p "$L"
rcon() { python3 tools/bench/rcon.py "$@" 2>/dev/null; }
pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
steam_running || { scripts/steam.sh >/dev/null; until grep -q 'Logged On' "$STEAM_DIR/logs/connection_log.txt"; do sleep 2; done; sleep 10; }
[ "$PROVIDER" = tf2mt ] && { make -s frontend unixlib; scripts/layer-install.sh >/dev/null; }
cp "$TF2DIR/tf/cfg/config.cfg" "$L/config.cfg.orig"
trap '[ "$PROVIDER" = tf2mt ] && scripts/layer-uninstall.sh >/dev/null; cp "$L/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"' EXIT
: > "$TF2DIR/tf/console.log"
TF2MT_VSYNC=0 TF2MT_UNIX_LOG=$L/unix.log TF2MT_TAG=soak TF2MT_TRACE_DIR="Z:${L//\//\\}" \
  scripts/tf2.sh -insecure -condebug -usercon -windowed -noborder -w 1920 -h 1080 +sv_lan 1 +rcon_password tf2mt \
  +ip 127.0.0.1 +maxplayers 24 +map "${MAPS[0]}" >/dev/null
for _ in $(seq 60); do tf2_running && break; sleep 1; done
( while tf2_running; do
    if grep -E 'Connecting to matchmaking server|Connected to [0-9]' "$TF2DIR/tf/console.log" | grep -qvE '(127\.0\.0\.1|localhost|loopback)'; then
      echo "GUARD: non-local server connection — killing TF2"; for p in $(tf2_pids); do kill -9 "$p"; done; exit 0
    fi
    sleep 1
  done ) &
wait_spawn() { for _ in $(seq 150); do tail -n +"$1" "$TF2DIR/tf/console.log" | grep -q 'Client reached server_spawn' && return 0; tf2_running || return 2; sleep 2; done; return 1; }
wait_spawn 1 || { echo "first map did not load"; exit 1; }
echo -e "cycle\tmap\tbuffers\tbuffer_MB\ttextures\ttexture_MB\trss_MB" > "$L/soak.tsv"
for i in $(seq 1 "$N"); do
  m=${MAPS[$((i % ${#MAPS[@]}))]}
  n=$(wc -l < "$TF2DIR/tf/console.log" | tr -d " ")
  rcon "changelevel $m" >/dev/null
  wait_spawn "$n"; rc=$?
  [ $rc = 2 ] && { echo "CRASH: TF2 exited during cycle $i ($m)"; break; }
  [ $rc = 1 ] && { echo "cycle $i: $m did not load"; continue; }
  sleep 12   # let the ledger print (every 600 frames) after the load settles
  live=$(grep 'ledger:' "$L/tf2mt-soak.log" | tail -1 | sed -E 's/.*live: ([0-9]+) buffers ([0-9.]+) MB, ([0-9]+) textures ([0-9.]+) MB.*/\1\t\2\t\3\t\4/')
  rss=$(ps -o rss= -p "$(tf2_pids | head -1)" 2>/dev/null | awk '{print int($1/1024)}')
  echo -e "$i\t$m\t$live\t$rss" | tee -a "$L/soak.tsv"
done
tf2_running && echo "survived $N map changes"
rcon quit >/dev/null; for _ in $(seq 30); do tf2_running || break; sleep 1; done
tf2_running && for p in $(tf2_pids); do kill "$p"; done
cp "$TF2DIR/tf/console.log" "$L/console.log"
