#!/bin/bash
. "$(dirname "$0")/_env.sh"
ok(){ [ -e "$1" ] && echo "ok   $1" || { echo "MISSING $1"; rc=1; }; }
rc=0
ok "$WINE"; ok "$STEAM_DIR/steam.exe"; ok "$STEAM_DIR/steamapps/appmanifest_440.acf"; ok "$TF2DIR/tf_win64.exe"
ok "$TF2DIR/bin/x64/shaderapidx9.dll"; ok "$TF2DIR/tf/cfg/config.cfg"
"$WINE" --version; "$WINE" cmd /c ver 2>/dev/null | tail -1; "$WINESERVER" -k 2>/dev/null; exit $rc
