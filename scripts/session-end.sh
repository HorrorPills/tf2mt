#!/bin/bash
# End the tf2mt session cleanly: Friends back online (if tf2.sh set them offline), Steam shut down, wineserver
# stopped, orphans swept, Metal renderer layer removed. Idempotent and locked: safe to run from several places.
. "$(dirname "$0")/_env.sh"
RUN="$TF2_HOME/run"; mkdir -p "$RUN"; LOG="$TF2_HOME/logs/session.log"; mkdir -p "$TF2_HOME/logs"
log() { echo "$(date '+%F %T') end[$$]: $*" >> "$LOG"; }
LOCK="$RUN/teardown.lock"
if ! mkdir "$LOCK" 2>/dev/null; then
  other=$(cat "$LOCK/pid" 2>/dev/null)
  if [ -n "$other" ] && kill -0 "$other" 2>/dev/null; then log "teardown already running ($other)"; exit 0; fi
  rm -rf "$LOCK"; mkdir "$LOCK" || exit 0
fi
echo $$ > "$LOCK/pid"; trap 'rm -rf "$LOCK"' EXIT
if tf2_running; then log "TF2 is running: nothing to do"; exit 0; fi
server_up() { pgrep -f "$TF2_HOME/wine/.*wineserver" >/dev/null; }
log "start (steam running: $(steam_running && echo yes || echo no))"
if [ -f "$RUN/friends-offline" ]; then
  if steam_running; then (cd "$STEAM_DIR" && "$WINE" steam.exe steam://friends/status/online >/dev/null 2>&1); sleep 3; log "friends set online"; fi
  rm -f "$RUN/friends-offline"
fi
if steam_running; then
  (cd "$STEAM_DIR" && "$WINE" steam.exe -shutdown >/dev/null 2>&1) &
  for _ in $(seq 30); do server_up || break; sleep 1; done
fi
if server_up; then log "wineserver still up: wineserver -k"; "$WINESERVER" -k 2>/dev/null; for _ in $(seq 10); do server_up || break; sleep 1; done; fi
"$(dirname "$0")/wine-sweep.sh" >> "$LOG" 2>&1
"$(dirname "$0")/layer-uninstall.sh" >/dev/null 2>&1
log "done (wineserver: $(server_up && echo STILL UP || echo gone))"
