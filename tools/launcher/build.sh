#!/bin/bash
# Build tf2mt.app (native SwiftUI launcher) and install it to /Applications (--no-install to only build).
# The app is self-contained and redistributable: it bundles the tf2mt scripts/configs/patch tool and an original
# icon. No Valve content is bundled — TF2 art and fonts are loaded at runtime from the user's own install.
set -euo pipefail
cd "$(dirname "$0")"; ROOT="$(cd ../.. && pwd)"
OUT="$ROOT/build/launcher"; APP="$OUT/tf2mt.app"; RES="$APP/Contents/Resources"
VERSION="$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo 0.1)"
rm -rf "$OUT"; mkdir -p "$APP/Contents/MacOS" "$RES/tf2mt/tools"
swiftc -O -parse-as-library -target arm64-apple-macos13 TF2Launcher.swift -o "$APP/Contents/MacOS/tf2mt"
"$ROOT/.venv/bin/python" make_icon.py "$OUT/tf2mt.iconset" "$OUT/icon-preview.png"
iconutil -c icns "$OUT/tf2mt.iconset" -o "$RES/tf2mt.icns"
# bundled tf2mt: only what the launcher runs (scripts, configs, wine patch tool, licence notes)
cp -R "$ROOT/scripts" "$ROOT/config" "$RES/tf2mt/"
cp -R "$ROOT/tools/wine-patches" "$RES/tf2mt/tools/"
cp "$ROOT/THIRD_PARTY.md" "$RES/tf2mt/"
find "$RES/tf2mt" -name '__pycache__' -prune -exec rm -rf {} +
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>tf2mt</string>
  <key>CFBundleDisplayName</key><string>tf2mt</string>
  <key>CFBundleIdentifier</key><string>local.tf2mt.launcher</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>tf2mt</string>
  <key>CFBundleIconFile</key><string>tf2mt</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
codesign -f -s - --deep "$APP" >/dev/null 2>&1
(cd "$OUT" && ditto -c -k --keepParent tf2mt.app tf2mt-app.zip)   # release asset
echo "built $APP  (+ $OUT/tf2mt-app.zip)"
if [ "${1:-}" != --no-install ]; then
  rm -rf "/Applications/Team Fortress 2.app" "/Applications/tf2mt.app"; cp -R "$APP" /Applications/
  /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f /Applications/tf2mt.app
  echo "installed /Applications/tf2mt.app"
fi
