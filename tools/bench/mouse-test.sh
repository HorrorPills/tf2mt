#!/bin/bash
# Mouse-input rate test: launch TF2 behind the trace proxy (tuned DXVK, vsync), owner turns the mouse
# (no keys) ~15 s in a map, then quits. Analysis: camera-matrix changes per second vs frames per second.
#   tools/bench/mouse-test.sh <tag>      (extra env passes through, e.g. DXVK_CONFIG_FILE)
cd "$(dirname "$0")/../.."; . scripts/_env.sh
tag=$1; R=$TF2_HOME/logs/runs/$tag; rm -rf "$R"; mkdir -p "$R"
pkill -f 'Team Fortress 2.tf_win64'; while pgrep -f 'Team Fortress 2.tf_win64' >/dev/null; do sleep 1; done
cp build/trace/d3d9.dll "$TF2DIR/d3d9.dll"; cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2DIR/d3d9_ref.dll"
TF2MT_TAG=$tag TF2MT_TRACE_DIR="Z:${R//\//\\}" DXVK_CONFIG_FILE="${DXVK_CONFIG_FILE:-$TF2_HOME/config/dxvk.conf}" scripts/tf2.sh "$@" >/dev/null
( until pgrep -f 'Team Fortress 2.tf_win64' >/dev/null; do sleep 1; done
  while pgrep -f 'Team Fortress 2.tf_win64' >/dev/null; do sleep 3; done
  rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll" ) >/dev/null 2>&1 & disown
echo "TF2 starting for mouse test '$tag'"
