#!/bin/bash
# Generate a bot navigation mesh for a map TF2 ships without one (koth_harvest_final has none: bots stay in spawn).
#   tools/bench/gen-nav.sh [map]   -> $TF2DIR/tf/maps/<map>.nav   (local listen server, offline)
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
map=${1:-koth_harvest_final}; nav="$TF2DIR/tf/maps/$map.nav"
[ -f "$nav" ] && { echo "exists: $nav"; exit 0; }
rcon() { python3 tools/bench/rcon.py "$@" 2>&1; }
pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
ensure_steam || { echo "Steam did not log in"; exit 1; }
cp "$TF2DIR/tf/cfg/config.cfg" /tmp/tf2mt-config.cfg.orig
trap 'cp /tmp/tf2mt-config.cfg.orig "$TF2DIR/tf/cfg/config.cfg"' EXIT
: > "$TF2DIR/tf/console.log"
TF2_FRIENDS_ONLINE=1 scripts/tf2.sh -insecure -condebug -usercon -windowed -noborder -w 1280 -h 720 +sv_lan 1 +rcon_password tf2mt \
  +ip 127.0.0.1 +maxplayers 24 +map "$map" >/dev/null
for _ in $(seq 60); do tf2_running && break; sleep 1; done
( while tf2_running; do   # never stay on a non-local server
    if grep -E 'Connecting to matchmaking server|Connected to [0-9]' "$TF2DIR/tf/console.log" | grep -qvE '(127\.0\.0\.1|localhost|loopback)'; then
      for p in $(tf2_pids); do kill -9 "$p"; done; exit 0; fi; sleep 1; done ) &
for _ in $(seq 150); do grep -q 'Client reached server_spawn' "$TF2DIR/tf/console.log" && break; sleep 2; done
sleep 8
rcon 'sv_cheats 1' 'nav_generate' >/dev/null
for i in $(seq 360); do [ -f "$nav" ] && break; tf2_running || break; sleep 5; done
sleep 5; ls -la "$nav" 2>/dev/null || { echo "no nav file; console tail:"; tail -20 "$TF2DIR/tf/console.log"; }
grep -a -i -E "nav|navigation" "$TF2DIR/tf/console.log" | tail -5
python3 tools/bench/rcon.py quit >/dev/null 2>&1; for _ in $(seq 30); do tf2_running || break; sleep 1; done; tf2_running && for p in $(tf2_pids); do kill "$p"; done
