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

# PIDs of the running game: only processes whose argv[0] is the Windows exe path (avoids matching shells or
# scripts that merely mention the name)
tf2_pids() { pgrep -fl 'tf_win64\.exe' | awk '$2 ~ /^C:/ {print $1}'; }
tf2_running() { [ -n "$(tf2_pids)" ]; }
# Steam client running in the prefix: argv[0] is steam.exe (with or without -silent)
steam_running() { pgrep -fl 'steam\.exe' | awk '$2 ~ /(^|\\)steam\.exe$/ {f=1} END {exit !f}'; }
