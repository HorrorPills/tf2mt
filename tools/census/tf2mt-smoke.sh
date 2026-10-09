#!/bin/bash
# M3 smoke test: TF2 with tf2mt as the ONLY D3D9 provider (no DXVK, no oracle) on a local map.
# Prints Source's rendering-path cvars, UNSEEN/UNIMPL events, and presented-frame statistics. Always -insecure.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
tag=${1:-m3smoke}; L=$TF2_HOME/logs/$tag; rm -rf "$L"; mkdir -p "$L"
pkill -f 'Team Fortress 2.tf_win64'; while tf2_running; do sleep 1; done
ensure_steam || { echo "Steam did not log in"; exit 1; }
scripts/layer-install.sh >/dev/null
trap 'scripts/layer-uninstall.sh >/dev/null' EXIT
: > "$TF2DIR/tf/console.log"
TF2MT_VSYNC=${TF2MT_VSYNC:-0} TF2MT_UNIX_LOG=$L/unix.log TF2MT_TAG=$tag TF2MT_TRACE_DIR="Z:${L//\//\\}" TF2_FRIENDS_ONLINE=1 \
  scripts/tf2.sh -insecure -condebug -usercon -windowed -noborder -w 1920 -h 1080 +sv_lan 1 +rcon_password tf2mt +ip 127.0.0.1 \
  $([ -n "${DEMO:-}" ] && echo "+fps_max 0 +demo_quitafterplayback 1 +playdemo bench" || echo "+map koth_harvest_final") "${@}" >/dev/null
if [ -n "${DEMO:-}" ]; then   # demo mode: wait for playback to finish (it quits itself)
  for _ in $(seq 60); do tf2_running && break; sleep 1; done
  for _ in $(seq 400); do tf2_running || break; sleep 2; done
  cp "$TF2DIR/tf/console.log" "$L/console.log"; grep -ah 'present' "$L/unix.log" "$L"/tf2mt-*.log 2>/dev/null | tail -6; exit 0
fi
for _ in $(seq 60); do tf2_running && break; sleep 1; done
for _ in $(seq 100); do grep -q 'Client reached server_spawn' "$TF2DIR/tf/console.log" && break; tf2_running || { echo "TF2 exited before the map loaded"; break; }; sleep 2; done
sleep 12
python3 tools/bench/rcon.py 'mat_dxlevel' 'mat_hdr_level' 'mat_queue_mode' 'mat_antialias' 2>&1 | grep -aE '^"' | cut -c1-80
python3 tools/bench/rcon.py quit >/dev/null 2>&1
for _ in $(seq 30); do tf2_running || break; sleep 1; done; tf2_running && for p in $(tf2_pids); do kill $p; done
cp "$TF2DIR/tf/console.log" "$L/console.log"
echo "--- tf2mt log (UNSEEN/UNIMPL/attach):"; grep -hE 'UNSEEN|UNIMPL|attach|adapter|CreateDevice' "$L"/tf2mt-*.log | sort | uniq -c | head -25
echo "--- present stats:"; grep '^present' "$L/unix.log" | tail -4
