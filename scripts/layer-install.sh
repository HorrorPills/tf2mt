#!/bin/bash
# Install the tf2mt renderer (ADR-002): tf2mt.dll + tf2mt.so as a new Wine builtin pair in the runtime, and a
# d3d9.dll forwarder beside tf_win64.exe. Wine's own d3d9 and DXVK are not modified. Undo: layer-uninstall.sh
set -eu
. "$(dirname "$0")/_env.sh"
B="$TF2MT_ROOT/build/tf2mt"
for f in tf2mt.dll tf2mt.so d3d9.dll; do [ -f "$B/$f" ] || { echo "missing $B/$f — run: make frontend unixlib"; exit 1; }; done
L="$TF2_HOME/wine/lib/wine"
cp "$B/tf2mt.dll" "$L/x86_64-windows/tf2mt.dll"
cp "$B/tf2mt.so" "$L/x86_64-unix/tf2mt.so"
cp "$B/tf2mt.dll" "$WINEPREFIX/drive_c/windows/system32/tf2mt.dll"   # Wine resolves builtins by name via system32
cp "$B/d3d9.dll" "$TF2DIR/d3d9.dll"
# a downloaded tf2mt.app passes the quarantine flag on to copied files, which macOS may block from loading
xattr -d com.apple.quarantine "$L/x86_64-windows/tf2mt.dll" "$L/x86_64-unix/tf2mt.so" \
  "$WINEPREFIX/drive_c/windows/system32/tf2mt.dll" "$TF2DIR/d3d9.dll" 2>/dev/null || true
