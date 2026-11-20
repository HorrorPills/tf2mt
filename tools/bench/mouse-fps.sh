#!/bin/bash
# Issue #2 test: does mouse input change the frame rate? Local listen server (koth_harvest_final, offline), free-roam
# spectator, synthetic mouse motion (build/tools/mousedrive) in on/off phases, per-phase fps from the frame logs.
#   tools/bench/mouse-fps.sh <name> [KEY=VAL ...]
#     RENDERER=tf2mt|dxvk  HZ=<display Hz, restored after>  FPS=<fps_max, 0 = uncapped>  VSYNC=0|1
#     MODE=borderless|fullscreen  PHASES=off:8,on:8,...  RATE=<mouse events/s>  DX=<counts per event>
#     DRIVER=hiddrive|mousedrive (HID relative motion = like a real mouse; mousedrive = synthetic CGEvents)
#     CHECK=<file> sources a behaviour-check script in game instead of the phases (tools/bench/relmouse-checks.sh)
#     ARGS="<extra TF2 args>"  (any other KEY=VAL is exported, e.g. TF2MT_DRAWABLES=3)
# Output: ~/Games/tf2/logs/mousefps/<name>/ (phases.txt, frames.csv, latency.csv, unix.log, summary.txt)
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
name=$1; shift
CHECK="" DRIVER=hiddrive RENDERER=tf2mt HZ="" FPS=120 VSYNC=0 MODE=borderless PHASES="off:8,on:8,off:8,on:8,off:6" RATE=500 DX=3 ARGS=""
for kv in "$@"; do case "${kv%%=*}" in CHECK|DRIVER|RENDERER|HZ|FPS|VSYNC|MODE|PHASES|RATE|DX|ARGS) eval "${kv%%=*}=\"\${kv#*=}\"" ;; *) export "$kv" ;; esac; done
out="$TF2_HOME/logs/mousefps/$name"; rm -rf "$out"; mkdir -p "$out"
state=$(build/tools/displaystate) || { echo "$name: display asleep/locked ($state)"; exit 1; }
rcon() { python3 tools/bench/rcon.py "$@" >/dev/null 2>&1; }
pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
ensure_steam || { echo "$name: Steam did not log in"; exit 1; }
hz0=$(build/tools/refresh get)
cp "$TF2DIR/tf/cfg/config.cfg" "$out/config.cfg.orig"
trap 'cp "$out/config.cfg.orig" "$TF2DIR/tf/cfg/config.cfg"; build/tools/refresh set "$hz0" >/dev/null; caffeinate_pid=${cpid:-}; [ -n "$caffeinate_pid" ] && kill $caffeinate_pid 2>/dev/null' EXIT
caffeinate -d & cpid=$!
[ -n "$HZ" ] && build/tools/refresh set "$HZ" >/dev/null && sleep 2
echo "display $(build/tools/refresh get) Hz" > "$out/setup.txt"
: > "$TF2DIR/tf/console.log"
case $MODE in fullscreen) win="-fullscreen -w 1920 -h 1080" ;; *) win="-windowed -noborder -w 1920 -h 1080" ;; esac
env_r=(TF2_FRIENDS_ONLINE=1)
if [ "$RENDERER" = tf2mt ]; then
  env_r+=(TF2_RENDERER=tf2mt TF2MT_VSYNC=$VSYNC TF2MT_LATENCY_CSV="$out/latency.csv")
else
  env_r+=(TF2_RENDERER=dxvk TF2_FRAMELOG=1 DXVK_CONFIG_FILE="$TF2_HOME/config/$([ "$VSYNC" = 1 ] && echo dxvk.conf || echo dxvk-immediate.conf)")
fi
before_m=$(ls -t "$TF2_HOME/logs/tf2mt" 2>/dev/null | head -1); before_d=$(ls -t "$TF2_HOME/logs/dxvk" 2>/dev/null | head -1)
env "${env_r[@]}" scripts/tf2.sh -insecure -condebug -usercon $win +sv_lan 1 +rcon_password tf2mt +ip 127.0.0.1 \
  +sv_rcon_banpenalty 0 +sv_rcon_maxfailures 20 +sv_rcon_minfailures 20 +fps_max "$FPS" $ARGS +map koth_harvest_final >/dev/null
for _ in $(seq 60); do tf2_running && break; sleep 1; done
( while tf2_running; do   # safety: never stay on a non-local server
    if grep -E 'Connecting to matchmaking server|Connected to [0-9]' "$TF2DIR/tf/console.log" | grep -qvE '(127\.0\.0\.1|localhost|loopback)'; then
      for p in $(tf2_pids); do kill -9 "$p"; done; exit 0; fi; sleep 1; done ) &
for _ in $(seq 150); do grep -q 'Client reached server_spawn' "$TF2DIR/tf/console.log" && break; tf2_running || break; sleep 2; done
sleep 10
# bring the game to the front so it owns the mouse
pid=$(tf2_pids | head -1)
osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $pid) to true" >/dev/null 2>&1
sleep 2
# Get into first-person play (RED Soldier). TF2's menus (MOTD, map info, team, class) appear in varying order, so:
# act, then probe (screenshot, short turn, screenshot): the camera turned = ready. Actions cycle until it does.
S="$out/probe"; mkdir -p "$S"
probe() {
  screencapture -x "$S/a.png"; build/tools/mousedrive 300 15 on:0.4 >/dev/null; sleep 0.3; screencapture -x "$S/b.png"
  local d; d=$(build/tools/imgdiff "$S/a.png" "$S/b.png"); echo "probe $1: $d" >> "$out/setup.txt"
  awk -v d="$d" 'BEGIN { exit !(d > 0.30) }'
}
ready=0
for i in $(seq 8); do
  case $((i % 3)) in
    1) build/tools/mousedrive click 1660 1023; sleep 3; build/tools/mousedrive click 1660 1023; sleep 3 ;;  # MOTD Continue, map info Close
    2) rcon 'sv_cheats 1' 'mp_waitingforplayers_cancel 1'
       build/tools/mousedrive key 21; sleep 3; build/tools/mousedrive key 19; sleep 4 ;;   # team menu "4" = RED, class menu "2" = Soldier
    0) build/tools/mousedrive key 53; sleep 3 ;;          # Escape (closes a menu, or opens/closes the main menu)
  esac
  probe "$i" && { ready=1; break; }
done
[ $ready = 1 ] || echo "WARNING: camera never turned in setup" | tee -a "$out/setup.txt"
rcon "fps_max $FPS"
python3 tools/bench/rcon.py 'fps_max' 2>/dev/null | grep -a '^ *"' > "$out/cvars.txt"
screencapture -x "$out/start.png" 2>/dev/null; sips -Z 640 "$out/start.png" >/dev/null 2>&1
sleep 2
if [ -n "${CHECK:-}" ]; then
  . "$CHECK"   # behaviour checks instead of the fps phases (tools/bench/relmouse-checks.sh)
else
  build/tools/${DRIVER:-hiddrive} "$RATE" "$DX" "$PHASES" > "$out/phases.txt"
  screencapture -x "$out/end.png" 2>/dev/null; sips -Z 640 "$out/end.png" >/dev/null 2>&1
  sleep 6   # the frame log is written in blocks: let the last phase reach the file
fi
rcon quit; for _ in $(seq 30); do tf2_running || break; sleep 1; done; tf2_running && for p in $(tf2_pids); do kill "$p"; done
sleep 2
if [ "$RENDERER" = tf2mt ]; then
  s=$(ls -t "$TF2_HOME/logs/tf2mt" | head -1); [ "$s" != "$before_m" ] && cp -R "$TF2_HOME/logs/tf2mt/$s/." "$out/"
else
  s=$(ls -t "$TF2_HOME/logs/dxvk" | head -1); [ "$s" != "$before_d" ] && cp -R "$TF2_HOME/logs/dxvk/$s/." "$out/"
fi
cp "$TF2DIR/tf/console.log" "$out/console.log"; cp "$TF2_HOME/logs/relmouse.log" "$out/" 2>/dev/null
[ -f "$out/phases.txt" ] && python3 tools/bench/mouse-fps.py "$out" | tee "$out/summary.txt"
[ -f "$out/checks.txt" ] && cat "$out/checks.txt"
