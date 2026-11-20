#!/bin/bash
# Issue #2 matrix: fps and latency while turning, relative mouse mode on vs off, over display rates, pacing modes and
# renderers. Each variant is one tools/bench/mouse-fps.sh run (~2 min). Summary: tools/bench/relmouse-summary.py
#   tools/bench/relmouse-matrix.sh <prefix> [variant ...]     variant = name:KEY=VAL,KEY=VAL  (default: the full matrix)
cd "$(dirname "$0")/../.."
prefix=$1; shift
variants=("$@")
if [ ${#variants[@]} = 0 ]; then
  for r in tf2mt dxvk; do for hz in 60 120 240; do for p in cap120:FPS=120,VSYNC=0 uncapped:FPS=0,VSYNC=0 vsync:FPS=0,VSYNC=1; do
    for fix in on:TF2_RELMOUSE=1 off:TF2_RELMOUSE=0; do
      variants+=("${r:0:1}${hz}-${p%%:*}-${fix%%:*}:RENDERER=$r,HZ=$hz,${p#*:},${fix#*:}")
    done; done; done; done
fi
for v in "${variants[@]}"; do
  name="$prefix-${v%%:*}"; IFS=, read -r -a kv <<< "${v#*:}"
  echo "== $name (${v#*:})"
  timeout 600 tools/bench/mouse-fps.sh "$name" "${kv[@]}" 2>&1 | grep -E 'verdict|WARNING|asleep|did not' || echo "   (no verdict)"
done
