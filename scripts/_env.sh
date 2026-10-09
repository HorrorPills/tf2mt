# Shared environment for all tf2mt scripts. Self-contained: works from a git checkout or from inside
# tf2mt.app (scripts are bundled). The game install location is configurable via TF2_HOME.
_tf2mt_src="${BASH_SOURCE[0]:-}"
[ -z "$_tf2mt_src" ] && [ -n "${ZSH_VERSION:-}" ] && eval '_tf2mt_src="${(%):-%x}"'   # sourced from zsh
TF2MT_ROOT="$(cd "$(dirname "$_tf2mt_src")/.." && pwd)"; export TF2MT_ROOT; unset _tf2mt_src
export TF2_HOME="${TF2_HOME:-$HOME/Games/tf2}"
export WINEPREFIX="$TF2_HOME/prefix"
export WINE="$TF2_HOME/wine/bin/wine"
export WINESERVER="$TF2_HOME/wine/bin/wineserver"
export STEAM_DIR="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
export TF2DIR="$STEAM_DIR/steamapps/common/Team Fortress 2"
export DYLD_FALLBACK_LIBRARY_PATH="$TF2_HOME/wine/lib:/usr/lib"   # FreeType etc. (lost if launched via SIP binaries like nohup)
export WINEDEBUG="${WINEDEBUG:--all}"
export MVK_CONFIG_LOG_LEVEL="${MVK_CONFIG_LOG_LEVEL:-0}"
# No Wine crash debugger: when TF2 crashes, winedbg (+ a conhost) stays attached to the dead game, keeping its
# network port open (the next TF2 then moves to another port) and processes alive. Steam still writes crash dumps.
case "${WINEDLLOVERRIDES:-}" in *winedbg*) ;; *) export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}winedbg.exe=d" ;; esac

# PIDs of the running game: only processes whose argv[0] is the Windows exe path (avoids matching shells or
# scripts that merely mention the name)
tf2_pids() { pgrep -fl 'tf_win64\.exe' | awk '$2 ~ /^C:/ {print $1}'; }
tf2_running() { [ -n "$(tf2_pids)" ]; }
# Steam client running in the prefix: argv[0] is steam.exe (with or without -silent)
steam_running() { pgrep -fl 'steam\.exe' | awk '$2 ~ /(^|\\)steam\.exe$/ {f=1} END {exit !f}'; }
# Start Steam if needed and wait for a NEW "Logged On" line (old lines from earlier sessions don't count). TF2 started
# before Steam has logged on crashes during load (engine.dll+0x960b3). Since v0.3.1 Steam is shut down after every
# session, so test scripts must use this rather than grepping the whole connection log.
ensure_steam() {
  steam_running && return 0
  local n; n=$(wc -l < "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null | tr -d ' '); n=${n:-0}
  "$TF2MT_ROOT/scripts/steam.sh" >/dev/null
  for _ in $(seq 120); do
    tail -n +$((n + 1)) "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null | grep -q 'Logged On' && { sleep 8; return 0; }
    sleep 2
  done
  return 1
}
