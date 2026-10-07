#!/bin/bash
# start the Steam client inside the TF2 Wine prefix (required before launching TF2)
#   scripts/steam.sh            background (no window) — normal use
#   scripts/steam.sh --login    with window — first login / account changes
. "$(dirname "$0")/_env.sh"; . "$(dirname "$0")/_tuned.sh"; cd "$STEAM_DIR" || exit 1
pgrep -f "$TF2_HOME/wine/.*wineserver" >/dev/null || { mkdir -p "$TF2_HOME/cache"; [ "${WINEMSYNC:-0}" = 1 ] && echo msync > "$TF2_SESSION_SYNC" || echo none > "$TF2_SESSION_SYNC"; }
follow_session_sync
silent=-silent; [ "${1:-}" = --login ] && silent=
mkdir -p "$TF2_HOME/logs"; "$WINE" steam.exe $silent -no-cef-sandbox >"$TF2_HOME/logs/steam.log" 2>&1 &
disown
echo "steam started (log: $TF2_HOME/logs/steam.log)"
