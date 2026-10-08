#!/bin/bash
# Remove the tf2mt renderer; TF2 goes back to DXVK.
. "$(dirname "$0")/_env.sh"
L="$TF2_HOME/wine/lib/wine"
rm -f "$TF2DIR/d3d9.dll" "$TF2DIR/d3d9_oracle.dll" "$L/x86_64-windows/tf2mt.dll" "$L/x86_64-unix/tf2mt.so" "$WINEPREFIX/drive_c/windows/system32/tf2mt.dll"
echo "tf2mt renderer removed (DXVK active)"
