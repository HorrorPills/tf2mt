#!/bin/bash
# Adapter parity test: tf2mt.dll must answer every IDirect3D9 question exactly as DXVK answered TF2 in the census
# (identity, 304 caps bytes, every recorded CheckDevice* query, display modes). NOTE: DXVK probed from a plain exe is
# NOT the reference — it reports a different identity/caps than inside TF2 (GeForce 8800 GTX vs Apple), so the census
# data (what TF2 really saw) is the yardstick.   usage: tools/census/adapter-parity.sh [census_variant, default caps2]
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
O=$TF2MT_ROOT/build/parity; mkdir -p "$O"; rm -f "$O"/tf2mt.*
win() { echo "Z:${1//\//\\}"; }
make -s probe frontend unixlib >/dev/null 2>&1
scripts/layer-install.sh >/dev/null
trap 'scripts/layer-uninstall.sh >/dev/null' EXIT
cp build/tools/probe.exe "$TF2_HOME/cache/probe.exe"
"$WINE" "$(win "$TF2_HOME/cache/probe.exe")" tf2mt.dll "$(win "$O/tf2mt.tsv")" "$(win "$TF2MT_ROOT/tests/golden/adapter_queries.tsv")" >"$O/tf2mt.log" 2>&1 &
pid=$!; n=0
while kill -0 $pid 2>/dev/null && [ $n -lt 120 ]; do sleep 1; n=$((n + 1)); done
kill -0 $pid 2>/dev/null && { echo "probe timed out"; kill $pid; exit 2; }
[ -s "$O/tf2mt.tsv" ] || { echo "no probe output"; cat "$O/tf2mt.log" | head; exit 2; }
.venv/bin/python tools/census/check_parity.py "$O/tf2mt.tsv" "$TF2_HOME/logs/census" "${1:-caps2}"
