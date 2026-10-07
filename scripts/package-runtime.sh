#!/bin/bash
# Pack the Wine runtime ($TF2_HOME/wine) as a tf2mt release asset with the ORIGINAL winemac.so
# (the mouse patch is applied on each user's machine by setup.sh). Prints URL/SHA lines for config/release.conf.
set -euo pipefail
. "$(dirname "$0")/_env.sh"
out="$TF2MT_ROOT/build/release"; mkdir -p "$out"
ver="wine$("$WINE" --version | sed 's/wine-//; s/ .*//')-$(cat "$TF2_HOME/wine/version" 2>/dev/null || echo x)"
name="tf2mt-runtime-$ver.tar.xz"
stage="$(mktemp -d)"; trap 'rm -rf "$stage"' EXIT
cp -cR "$TF2_HOME/wine" "$stage/wine"              # APFS clone: instant, no extra space
orig="$TF2_HOME/backups/winemac.so.orig"
if [ -f "$orig" ]; then cp "$orig" "$stage/wine/lib/wine/x86_64-unix/winemac.so"; fi
TF2_HOME="$stage" /usr/bin/python3 "$TF2MT_ROOT/tools/wine-patches/winemac_warp_nodiscard.py" status | grep -q '^original' \
  || { echo "staged winemac.so is not the original build — refusing to package"; exit 1; }
cp "$TF2MT_ROOT/THIRD_PARTY.md" "$stage/wine/THIRD_PARTY.md"
echo "packing $name (xz, may take a few minutes)…"
tar -C "$stage" -cf - wine | xz -T0 -6 > "$out/$name"
sha=$(shasum -a 256 "$out/$name" | cut -d' ' -f1)
echo "$sha  $name" > "$out/$name.sha256"
ls -lh "$out/$name"
echo "Upload $out/$name to a GitHub release, then set in config/release.conf:"
echo "RUNTIME_URL=\"https://github.com/<owner>/tf2mt/releases/download/<tag>/$name\""
echo "RUNTIME_SHA256=\"$sha\""
