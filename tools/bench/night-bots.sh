#!/bin/bash
# Bot-match run (koth_harvest_final, 22 bots fighting, first-person spectating) on the Metal renderer.
#   night-bots.sh <name> [KEY=VAL ...]   SECS=240 (play time), WARP=1|events, VSYNC=0|1, PRESET=<mastercomfig preset>
# Output: ~/Games/tf2/logs/night/<name>/ (+ kills.txt). Offline listen server only; kills TF2 on any non-local connect.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
name=$1; shift
for kv in "$@"; do export "$kv"; done
out="$TF2_HOME/logs/night/$name"; rm -rf "$out"; mkdir -p "$out"
state=$(build/tools/displaystate) || { echo "$name: display asleep/locked ($state)"; exit 1; }
rcon() { python3 tools/bench/rcon.py "$@" >/dev/null 2>&1; }
pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
ensure_steam || { echo "$name: Steam did not log in"; exit 1; }
cp "$TF2DIR/tf/cfg/config.cfg" "$out/config.cfg.orig"
trap 'cp "$out/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"' EXIT
: > "$TF2DIR/tf/console.log"
before=$(ls -t "$TF2_HOME/logs/tf2mt" 2>/dev/null | head -1)
TF2_FRIENDS_ONLINE=1 TF2_RENDERER=tf2mt TF2MT_VSYNC=${VSYNC:-1} TF2MT_LATENCY_CSV="$out/latency.csv" \
  scripts/tf2.sh -insecure -condebug -usercon -windowed -noborder -w 1920 -h 1080 +sv_lan 1 +rcon_password tf2mt \
  +ip 127.0.0.1 +sv_rcon_banpenalty 0 +sv_rcon_maxfailures 20 +sv_rcon_minfailures 20 +maxplayers 24 +map koth_harvest_final >/dev/null
for _ in $(seq 60); do tf2_running && break; sleep 1; done
( while tf2_running; do
    if grep -E 'Connecting to matchmaking server|Connected to [0-9]' "$TF2DIR/tf/console.log" | grep -qvE '(127\.0\.0\.1|localhost|loopback)'; then
      for p in $(tf2_pids); do kill -9 "$p"; done; exit 0; fi; sleep 1; done ) &
for _ in $(seq 150); do grep -q 'Client reached server_spawn' "$TF2DIR/tf/console.log" && break; tf2_running || break; sleep 2; done
sleep 8
# heavier presets finish loading later and RCON may not be up yet: retry until bots have actually joined
for attempt in 1 2 3 4 5 6; do
  rcon 'sv_cheats 1' 'mp_autoteambalance 0' 'tf_bot_quota_mode fill' 'tf_bot_quota 22' 'mp_waitingforplayers_cancel 1' \
       'mp_timelimit 0' 'mp_winlimit 0' 'tf_bot_difficulty 2' 'jointeam spectator'
  sleep 15
  [ "$(python3 tools/bench/rcon.py status 2>/dev/null | grep -c BOT)" -ge 10 ] && break
  echo "$name: bots not in yet (attempt $attempt)"; sleep 10
done
echo "bots in game: $(python3 tools/bench/rcon.py status 2>/dev/null | grep -c BOT)" > "$out/bots.txt"
sleep 10
if [ "${WARP:-0}" != 0 ]; then
  ( while tf2_running; do build/tools/cursorwarp 120 30 $([ "$WARP" = events ] && echo events); done ) & fi
end=$((SECONDS + ${SECS:-240})); k0=$(grep -c ' killed ' "$TF2DIR/tf/console.log")
while [ $SECONDS -lt $end ] && tf2_running; do rcon 'spec_mode 4' 'spec_next'; sleep 15; done
echo "kills during measurement: $(( $(grep -c ' killed ' "$TF2DIR/tf/console.log") - k0 ))" > "$out/kills.txt"
rcon quit; for _ in $(seq 30); do tf2_running || break; sleep 1; done; tf2_running && for p in $(tf2_pids); do kill "$p"; done
pkill -f cursorwarp 2>/dev/null; wait 2>/dev/null
sess=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1)
[ "$sess" != "$before" ] && cp -R "$TF2_HOME/logs/tf2mt/$sess/." "$out/"
cp "$TF2DIR/tf/console.log" "$out/console.log"
echo "$name: done $(date +%H:%M:%S), $(cat "$out/kills.txt")"
