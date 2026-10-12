#!/bin/bash
# Run a Windows program in TF2's Wine prefix (same runtime and environment as the game): scripts/run-exe.sh <file.exe> [args…]
# Launcher: Debug → "Load .exe…". Output goes to logs/run-exe.log.
. "$(dirname "$0")/_env.sh"; . "$(dirname "$0")/_tuned.sh"
exe=${1:?usage: run-exe.sh <file.exe> [args…]}; shift
[ -f "$exe" ] || { echo "not found: $exe"; exit 1; }
mkdir -p "$TF2_HOME/logs" "$TF2_HOME/cache"
# join a running session with its sync mode (a wineserver only accepts clients using the same one)
pgrep -f "$TF2_HOME/wine/.*wineserver" >/dev/null || { [ "${WINEMSYNC:-0}" = 1 ] && echo msync > "$TF2_SESSION_SYNC" || echo none > "$TF2_SESSION_SYNC"; }
follow_session_sync
cd "$(dirname "$exe")" || exit 1
echo "== $(date '+%F %T') $exe $*" >> "$TF2_HOME/logs/run-exe.log"
"$WINE" "$exe" "$@" >>"$TF2_HOME/logs/run-exe.log" 2>&1 &
disown
echo "started $(basename "$exe")"
