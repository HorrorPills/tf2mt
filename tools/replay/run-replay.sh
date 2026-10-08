#!/bin/bash
# Replay a .t9 capture against DXVK (oracle) or tf2mt (under test) inside the game's Wine prefix.
#   tools/replay/run-replay.sh <capture.t9> <dxvk|tf2mt> [replay args, e.g. --frames 600]
# Env: VERIFY=1 turns on tf2mt's byte-exact upload verification (TF2MT_VERIFY_UPLOADS). TIMEOUT (s, default 3600).
#      DUMP=f1,f2,... writes the back buffer before those Presents to <capture dir>/frames-<provider>/frame-N.ppm.
# Output: <capture dir>/replay-<provider>[-verify].txt (report) and, for tf2mt, tf2mt-replay.log + unix.log.
set -u
cd "$(dirname "$0")/../.."; . scripts/_env.sh
cap=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); prov=$2; shift 2
dir=$(dirname "$cap")
win() { echo "Z:${1//\//\\}"; }
make -s replay >/dev/null
cp build/tools/replay.exe "$TF2_HOME/cache/replay.exe"
suffix=$prov${VERIFY:+-verify}
dumpargs=()
if [ -n "${DUMP:-}" ]; then mkdir -p "$dir/frames-$prov"; rm -f "$dir/frames-$prov"/*.ppm; dumpargs=(--dump "$DUMP" --dump-dir "$(win "$dir/frames-$prov")"); fi
case $prov in
  dxvk)
    cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2_HOME/cache/d3d9_dxvk.dll"
    provider=$(win "$TF2_HOME/cache/d3d9_dxvk.dll") ;;
  tf2mt)
    make -s frontend unixlib >/dev/null
    scripts/layer-install.sh >/dev/null
    trap 'scripts/layer-uninstall.sh >/dev/null' EXIT
    provider=tf2mt.dll
    export TF2MT_TRACE_DIR=$(win "$dir") TF2MT_TAG=replay TF2MT_UNIX_LOG="$dir/unix-$suffix.log" TF2MT_VSYNC=0 TF2MT_NO_DEFER=${TF2MT_NO_DEFER:-1}
    [ "${VERIFY:-}" = 1 ] && export TF2MT_VERIFY_UPLOADS=1
    export TF2MT_NO_LODBIAS=${TF2MT_NO_LODBIAS:-0} TF2MT_NO_ENCCACHE=${TF2MT_NO_ENCCACHE:-0}
    rm -f "$dir/unix-$suffix.log" ;;
  *) echo "provider must be dxvk or tf2mt"; exit 2 ;;
esac
"$WINE" "$(win "$TF2_HOME/cache/replay.exe")" "$provider" "$(win "$cap")" --report "$(win "$dir/replay-$suffix.txt")" ${dumpargs[@]+"${dumpargs[@]}"} "$@" \
  >"$dir/replay-$suffix.stdout" 2>&1 &
pid=$!; n=0
while kill -0 $pid 2>/dev/null && [ $n -lt "${TIMEOUT:-3600}" ]; do sleep 1; n=$((n + 1)); done
if kill -0 $pid 2>/dev/null; then echo "replay timed out after ${n}s"; kill $pid; exit 2; fi
wait $pid; rc=$?
[ "$prov" = tf2mt ] && cp "$dir/tf2mt-replay.log" "$dir/tf2mt-replay-$suffix.log" 2>/dev/null
tail -n 60 "$dir/replay-$suffix.txt" 2>/dev/null || tail -n 30 "$dir/replay-$suffix.stdout"
exit $rc
