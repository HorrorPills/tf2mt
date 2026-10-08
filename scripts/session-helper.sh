#!/bin/bash
# Per-session helper started by the launcher (never installed anywhere; exits when the session ends).
# Ends the session (scripts/session-end.sh) when:
#   * TF2 ran and has exited (whatever closed it: in-game quit, Dock, crash), or
#   * TF2 never started, no launch is in progress and the launcher is gone (e.g. only Steam was opened).
# Independent of the launcher: survives the launcher being quit or force-quit. One helper at a time.
#   session-helper.sh --spawn   start a helper in the background unless one is already running (returns at once)
. "$(dirname "$0")/_env.sh"
RUN="$TF2_HOME/run"; mkdir -p "$RUN" "$TF2_HOME/logs"; LOG="$TF2_HOME/logs/session.log"
log() { echo "$(date '+%F %T') helper[$$]: $*" >> "$LOG"; }
alive() { local p; p=$(cat "$1" 2>/dev/null) && [ -n "$p" ] && kill -0 "$p" 2>/dev/null; }
if [ "${1:-}" = --spawn ]; then
  alive "$RUN/helper.pid" && exit 0
  nohup /bin/bash "$0" --run </dev/null >/dev/null 2>&1 &
  exit 0
fi
[ "${1:-}" = --run ] || { echo "usage: $0 --spawn"; exit 2; }
alive "$RUN/helper.pid" && exit 0
echo $$ > "$RUN/helper.pid"; trap 'rm -f "$RUN/helper.pid"' EXIT
server_up() { pgrep -f "$TF2_HOME/wine/.*wineserver" >/dev/null; }
log "started"
seen=0; quiet=0; gone=0; started=$(date +%s)
end_session() {   # $1 = reason. Afterwards keep watching if a new launch already started (quit + Play right away)
  log "$1: ending session"; "$(dirname "$0")/session-end.sh"
  for _ in $(seq 5); do
    if alive "$RUN/launch-pending" || tf2_running; then log "new launch in progress: watching it"; seen=0; quiet=0; gone=0; started=$(date +%s); return; fi
    sleep 1
  done
  exit 0
}
while :; do
  sleep 1
  if tf2_running; then seen=1; quiet=0; gone=0; continue; fi
  if alive "$RUN/launch-pending"; then quiet=0; gone=0; continue; fi       # play.sh is starting Steam/TF2
  if ! server_up; then
    # nothing of ours runs (Steam not started yet, or the session already ended): give a just-requested Steam
    # start a moment to appear, then sweep leftovers and stop
    [ $(( $(date +%s) - started )) -lt 20 ] && [ $seen = 0 ] && continue
    "$(dirname "$0")/wine-sweep.sh" >> "$LOG" 2>&1; log "no wineserver: done"; exit 0
  fi
  quiet=$((quiet + 1))
  if [ $seen = 1 ]; then
    [ $quiet -ge 3 ] || continue                                          # TF2 gone for 3 s (not a restart)
    end_session "TF2 exited"; continue
  fi
  if alive "$RUN/launcher.pid"; then gone=0; continue; fi
  gone=$((gone + 1))
  [ $gone -ge 10 ] || continue                                            # launcher closed for 10 s, no TF2, no launch
  end_session "launcher closed without TF2 running"
done
