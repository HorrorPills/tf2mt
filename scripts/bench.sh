#!/bin/bash
# One benchmark run = one fresh TF2 process (PLAN.md F12) playing tf/bench.dem in real time.
#   scripts/bench.sh <tag> <ref> [extra launch args...]
#   <ref>: dxvk  (runtime DXVK 2.4.1 behind the timing proxy)
#          null  (build/null/d3d9.dll behind the timing proxy: game-only ceiling)
#          tf2mt (the tf2mt renderer behind the timing proxy; layer installed for the run, removed afterwards)
# Env passthrough: anything exported (DXVK_*, MVK_*, WINE*) reaches the game.
# Output: ~/Games/tf2/logs/runs/<tag>/{frames-<tag>.csv,.info.txt,threads-<tag>.csv,console.log,args.txt,report.txt,report.json}
# Validity: the display must be awake and the session unlocked for the whole run (a sleeping/locked
# screen stops composition and changes Present behaviour). The run is refused up front, the display is
# kept awake with caffeinate, and a run whose display state changed is marked INVALID.
set -u
export TF2_TUNED=0   # baselines use exactly the DXVK config given in the environment
. "$(dirname "$0")/_env.sh"
tag=$1; ref=$2; shift 2
run="$TF2_HOME/logs/runs/$tag"
[ -e "$run" ] && { echo "run tag '$tag' already used — pick a fresh one"; exit 1; }
state=$("$TF2MT_ROOT/build/tools/displaystate") || { echo "refusing to run: $state (display asleep or screen locked)"; exit 1; }
mkdir -p "$run"
caffeinate -d -w $$ &
cleanup() {   # leave the game folder (and, for tf2mt, the runtime) clean
  rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_ref.dll" "$TF2DIR/d3d9_oracle.dll"
  [ "$ref" = tf2mt ] && "$TF2MT_ROOT/scripts/layer-uninstall.sh" >/dev/null
}
trap cleanup EXIT

case $ref in
  dxvk) src="$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" ;;
  null) src="$TF2MT_ROOT/build/null/d3d9.dll" ;;
  tf2mt)
    make -C "$TF2MT_ROOT" -s frontend unixlib trace
    "$TF2MT_ROOT/scripts/layer-install.sh" >/dev/null
    src="$TF2MT_ROOT/build/tf2mt/d3d9.dll"     # forwarder -> builtin tf2mt.dll
    export TF2MT_UNIX_LOG="$run/unix.log" ;;
  *) echo "unknown ref '$ref'"; exit 1 ;;
esac
cp "$TF2MT_ROOT/build/trace/d3d9.dll" "$TF2DIR/d3d9.dll"
cp "$src" "$TF2DIR/d3d9_ref.dll"
cp "$TF2_HOME/wine/share/dxvk/x86_64-windows/d3d9.dll" "$TF2DIR/d3d9_oracle.dll"  # null renderer asks DXVK for caps

pkill -f 'Team Fortress 2.tf_win64' 2>/dev/null
while pgrep -f 'Team Fortress 2.tf_win64' >/dev/null; do sleep 1; done
# Steam must be logged on, otherwise TF2 runs -insecure and crashes during load
pgrep -f steamwebhelper >/dev/null || { "$TF2MT_ROOT/scripts/steam.sh"; }
until tail -n 30 "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null | grep -q 'Logged On'; do sleep 2; done

: > "$TF2DIR/tf/console.log"
args=(-insecure -condebug -windowed -noborder -w 1920 -h 1080 "$@" +demo_quitafterplayback 1 +playdemo bench)
printf '%s\n' "ref=$ref" "args=${args[*]}" > "$run/args.txt"
env | grep -E '^(DXVK_|MVK_|WINE[A-Z]*SYNC|TF2MT_)' >> "$run/args.txt"
echo "display_start=$state" >> "$run/args.txt"
echo "wine_session_sync=$(cat "$TF2_HOME/cache/wine-session-sync" 2>/dev/null || echo unknown)" >> "$run/args.txt"
export TF2MT_TAG=$tag TF2MT_TRACE_DIR="Z:${run//\//\\}"
"$TF2MT_ROOT/scripts/tf2.sh" "${args[@]}" >/dev/null

for _ in $(seq 120); do pid=$(pgrep -f 'Team Fortress 2.tf_win64' | head -1); [ -n "$pid" ] && break; sleep 1; done
[ -z "${pid:-}" ] && { echo "tf_win64 did not start"; exit 1; }
"$TF2MT_ROOT/build/tools/threadmon" "$pid" 1000 "$run/threads-$tag.csv" &
deadline=$((SECONDS + 900))
while kill -0 "$pid" 2>/dev/null; do
  [ $SECONDS -gt $deadline ] && { echo "timeout — killing"; kill "$pid"; }
  "$TF2MT_ROOT/build/tools/displaystate" >/dev/null || display_bad=1
  sleep 2
done
wait "$!" 2>/dev/null
if [ -n "${display_bad:-}" ]; then
  echo "INVALID: display slept or screen locked during the run" | tee "$run/INVALID"
fi
cp "$TF2DIR/tf/console.log" "$run/console.log"
"$TF2MT_ROOT/.venv/bin/python" "$TF2MT_ROOT/tools/bench/analyze.py" "$run" "$tag" --json "$run/report.json" | tee "$run/report.txt"
