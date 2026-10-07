#!/bin/bash
# Repro: local map, then open the loadout (character) screen via RCON; trace + threadmon + `sample` of the stall.
#   tools/bench/repro-loadout.sh <tag> <dxvk|null>
cd "$(dirname "$0")/../.."
. scripts/_env.sh
tag=$1; ref=$2; R=$TF2_HOME/logs/runs/$tag; mkdir -p "$R"
pkill -f 'Team Fortress 2.tf_win64'; while pgrep -f 'Team Fortress 2.tf_win64' >/dev/null; do sleep 1; done
until tail -n 30 "$STEAM_DIR/logs/connection_log.txt" | grep -q 'Logged On'; do sleep 2; done
cp build/trace/d3d9.dll "$TF2DIR/d3d9.dll"
cp "$([ "$ref" = null ] && echo build/null/d3d9.dll || echo "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll")" "$TF2DIR/d3d9_ref.dll"
cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2DIR/d3d9_oracle.dll"
trap 'rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll" "$TF2DIR/d3d9_oracle.dll"' EXIT
: > "$TF2DIR/tf/console.log"
TF2MT_TAG=$tag TF2MT_TRACE_DIR="Z:${R//\//\\}" scripts/tf2.sh -condebug -usercon -windowed -noborder -w 1920 -h 1080 \
  +sv_lan 1 +rcon_password tf2mt +ip 127.0.0.1 +maxplayers 24 +map koth_harvest_final >/dev/null
until pgrep -f 'Team Fortress 2.tf_win64' >/dev/null; do sleep 1; done
P=$(pgrep -f 'Team Fortress 2.tf_win64')
build/tools/threadmon "$P" 1000 "$R/threads-$tag.csv" &
WS=$(pgrep -f "bin/wineserver" | head -1); build/tools/threadmon "$WS" 1000 "$R/wineserver-$tag.csv" &
( while kill -0 "$P" 2>/dev/null; do ps -axo pid=,time=,args= | grep -E "C:\\\\|wineserver|MTLCompilerService|steam\.exe" | grep -v grep | sed "s/^/$(date +%s) /" | cut -c1-140; sleep 1; done ) > "$R/procs-$tag.txt" &
( while kill -0 "$P" 2>/dev/null; do echo "$(date +%s.%N | cut -c1-14) $(ioreg -r -d 1 -c IOAccelerator | grep -oE '"Device Utilization %"=[0-9]+' | cut -d= -f2)"; sleep 0.5; done ) > "$R/gpu-$tag.txt" &
until grep -q 'Client reached server_spawn' "$TF2DIR/tf/console.log"; do sleep 2; done
sleep 15
python3 tools/bench/rcon.py ${VPROF:+'vprof_on' 'vprof_dump_spikes 200'} ${PINGDBG:+'tf_datacenter_ping_debug 1'} ${CONTS:+'con_timestamp 1'} 'open_charinfo_direct' >/dev/null
sleep 4
: sample "$WS" 6 -file "$R/sample-wineserver.txt" >/dev/null 2>&1 &
: sample "$P" 8 -file "$R/sample-stall.txt" >/dev/null 2>&1
if [ -n "${STALL_SAMPLE:-}" ]; then   # sample 2 s only once VProf reports a >1 s frame
  end=$((SECONDS + ${LOADOUT_S:-30}))
  while [ $SECONDS -lt $end ]; do
    if grep -qE '^Peak [0-9]{4}' "$TF2DIR/tf/console.log"; then
      sample "$P" 2 1 -file "$R/sample-confirmed-stall.txt" >/dev/null 2>&1; vmmap "$P" > "$R/vmmap.txt" 2>/dev/null; break
    fi; sleep 1
  done
  [ $SECONDS -lt $end ] && sleep $((end - SECONDS))
else
  sleep "${LOADOUT_S:-30}"
fi
python3 tools/bench/rcon.py quit >/dev/null 2>&1
while kill -0 "$P" 2>/dev/null; do sleep 1; done
cp "$TF2DIR/tf/console.log" "$R/console.log"
grep -c SLOW "$R/frames-$tag.info.txt"
