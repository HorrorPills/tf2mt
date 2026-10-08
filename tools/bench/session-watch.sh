#!/bin/bash
# Live view of the tf2mt session state for quit-path tests. Prints a line whenever the state changes; exits once
# nothing of the session is left (after TF2 has been seen), or after the timeout.
. "$(dirname "$0")/../../scripts/_env.sh"
N="$TF2_HOME/wine/lib/wine/x86_64-unix/ntdll.so"; RUN="$TF2_HOME/run"; T=${1:-900}; seen=0; last=""; t0=$(date +%s)
alive() { local p; p=$(cat "$1" 2>/dev/null) && [ -n "$p" ] && kill -0 "$p" 2>/dev/null && echo yes || echo no; }
while [ $(( $(date +%s) - t0 )) -lt "$T" ]; do
  tf=$(tf2_running && echo yes || echo no); st=$(steam_running && echo yes || echo no)
  ws=$(pgrep -f "$TF2_HOME/wine/.*wineserver" >/dev/null && echo yes || echo no)
  n=$(lsof -t "$N" 2>/dev/null | wc -l | tr -d ' '); la=$(pgrep -f "tf2mt.app/Contents/MacOS/tf2mt" >/dev/null && echo yes || echo no)
  cur="TF2=$tf Steam=$st wineserver=$ws wine-procs=$n launcher=$la helper=$(alive "$RUN/helper.pid")"
  [ "$cur" != "$last" ] && { echo "$(date +%T) $cur"; last=$cur; }
  [ $tf = yes ] && seen=1
  if [ $seen = 1 ] && [ $tf = no ] && [ $ws = no ] && [ "$n" = 0 ]; then echo "$(date +%T) CLEAN: everything exited"; exit 0; fi
  sleep 1
done
echo "$(date +%T) TIMEOUT: still running: $cur"; ps -axo pid,etime,command | grep -E '[.]exe|[w]ineserver' | cut -c1-110
