#!/bin/bash
# tf2mt setup — idempotent: every step checks first and skips what is already done.
#   scripts/setup.sh [--runtime <path-or-url to tf2mt-runtime-*.tar.xz>]
# Steps: rosetta → wine runtime → prefix + DXVK → Steam (Windows) → tf2mt configs → mouse patch.
# Afterwards: log in to Steam once (scripts/steam.sh --login) and install TF2 (steam://install/440).
# Output lines starting with "==>" are step headers, "OK"/"SKIP"/"FAIL"/"TODO" are results (parsed by the launcher).
set -u
. "$(dirname "$0")/_env.sh"
. "$TF2MT_ROOT/config/release.conf" 2>/dev/null || true   # RUNTIME_URL / RUNTIME_SHA256 of the published runtime

RUNTIME_SRC="${TF2MT_RUNTIME:-${RUNTIME_URL:-}}"
while [ $# -gt 0 ]; do
  case $1 in --runtime) RUNTIME_SRC=$2; shift 2 ;; *) echo "unknown option $1"; exit 2 ;; esac
done

step() { echo "==> $1"; }
ok()   { echo "OK   $1"; }
skip() { echo "SKIP $1"; }
todo() { echo "TODO $1"; }
fail() { echo "FAIL $1"; exit 1; }

mkdir -p "$TF2_HOME"/{logs,config,cache,backups}

step "Rosetta 2"
if [ -e /Library/Apple/usr/libexec/oah/libRosettaRuntime ]; then skip "installed"
else
  softwareupdate --install-rosetta --agree-to-license >/dev/null 2>&1 \
    && ok "installed" || fail "could not install Rosetta — run: softwareupdate --install-rosetta --agree-to-license"
fi

step "Wine runtime ($TF2_HOME/wine)"
if [ -x "$WINE" ]; then skip "present ($("$WINE" --version 2>/dev/null))"
else
  [ -n "$RUNTIME_SRC" ] || fail "no runtime source — pass --runtime <tf2mt-runtime-*.tar.xz path or URL> (see README)"
  tarball="$RUNTIME_SRC"
  if [[ $RUNTIME_SRC == http* ]]; then
    tarball="$TF2_HOME/cache/$(basename "$RUNTIME_SRC")"
    echo "     downloading $RUNTIME_SRC"
    curl -fsSL --retry 3 -o "$tarball.part" "$RUNTIME_SRC" && mv "$tarball.part" "$tarball" || fail "download failed"
  fi
  if [ -n "${RUNTIME_SHA256:-}" ]; then
    echo "$RUNTIME_SHA256  $tarball" | shasum -a 256 -c - >/dev/null || fail "runtime checksum mismatch"
  fi
  tar -xJf "$tarball" -C "$TF2_HOME" || fail "could not unpack $tarball"
  xattr -dr com.apple.quarantine "$TF2_HOME/wine" 2>/dev/null
  [ -x "$WINE" ] && ok "installed ($("$WINE" --version))" || fail "runtime unpacked but $WINE missing"
fi

step "Wine prefix + DXVK ($WINEPREFIX)"
if [ ! -f "$WINEPREFIX/system.reg" ]; then
  WINEDLLOVERRIDES="mscoree,mshtml=" "$WINE" wineboot -u >/dev/null 2>&1
  "$WINESERVER" -w
  [ -f "$WINEPREFIX/system.reg" ] || fail "wineboot did not create the prefix"
fi
dx="$TF2_HOME/wine/share/dxvk"
cp "$dx/x86_64-windows/d3d9.dll" "$WINEPREFIX/drive_c/windows/system32/d3d9.dll" || fail "DXVK x64 copy"
cp "$dx/i386-windows/d3d9.dll" "$WINEPREFIX/drive_c/windows/syswow64/d3d9.dll" || fail "DXVK x86 copy"
"$WINE" reg add 'HKCU\Software\Wine\AppDefaults\tf_win64.exe\DllOverrides' /v d3d9 /t REG_SZ /d native /f >/dev/null 2>&1 \
  || fail "registry override"
ok "prefix ready, DXVK d3d9 installed for tf_win64.exe"

step "Steam for Windows"
if [ -f "$STEAM_DIR/steam.exe" ]; then skip "installed"
else
  inst="$TF2_HOME/cache/SteamSetup.exe"
  curl -fsSL --retry 3 -o "$inst" https://cdn.cloudflare.steamstatic.com/client/installer/SteamSetup.exe || fail "Steam installer download failed"
  "$WINE" "$inst" /S >/dev/null 2>&1      # NSIS silent install; the installer starts Steam afterwards
  for _ in $(seq 60); do [ -f "$STEAM_DIR/steam.exe" ] && break; sleep 2; done
  "$WINESERVER" -k 2>/dev/null           # stop the auto-started Steam; first login is done via the launcher
  [ -f "$STEAM_DIR/steam.exe" ] && ok "installed" || fail "Steam installer did not finish"
fi

step "tf2mt configs"
for f in dxvk.conf dxvk-immediate.conf; do cp "$TF2MT_ROOT/config/$f" "$TF2_HOME/config/$f"; done
ok "DXVK configs → $TF2_HOME/config"

step "Smooth mouse fix (Wine patch)"
st=$(/bin/bash "$TF2MT_ROOT/tools/wine-patches/winemac_warp_nodiscard.sh" status 2>&1)
case $st in
  patched*)  skip "already applied" ;;
  original*) /bin/bash "$TF2MT_ROOT/tools/wine-patches/winemac_warp_nodiscard.sh" apply >/dev/null && ok "applied" || fail "patch failed" ;;
  *)         todo "this Wine build is not the tested one — patch not applied (mouse may feel ~40 Hz)" ;;
esac

step "Steam login + Team Fortress 2"
if grep -q '"StateFlags"[[:space:]]*"4"' "$STEAM_DIR/steamapps/appmanifest_440.acf" 2>/dev/null; then skip "TF2 installed"
elif grep -q 'Logged On' "$STEAM_DIR/logs/connection_log.txt" 2>/dev/null; then todo "install TF2: in the launcher click “Install TF2” (≈31 GB)"
else todo "log in to Steam once: in the launcher click “Log in to Steam”, then “Install TF2”"
fi
echo "DONE"
