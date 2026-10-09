#!/bin/bash
# mastercomfig graphics presets (https://github.com/mastercomfig/mastercomfig), used by the launcher.
#   scripts/mastercomfig.sh status          -> "installed=<yes|no> preset=<name|none>"
#   scripts/mastercomfig.sh set <preset>    -> low | balanced | medium | high | ultra (also: destitute, custom)
# "balanced" (tf2mt): mastercomfig Medium with its four costliest module groups at their Low level (shadows, water,
# post-processing, anti-aliasing), written to cfg/overrides/modules.cfg (marked as tf2mt's; a user's own file there
# is backed up and put back when another preset is chosen). Measured 2026-10-09 (docs/overnight-2026-10-09.md):
# bot match at 120 Hz 119.2 fps / 52 visible jumps per min vs Medium 114.1 / 354.
# mastercomfig 9.x selects its preset from a `preset=<name>` line in cfg/app/setup_hook.cfg (the comfig app's
# file). `set` rewrites only that line (addons and other settings stay), and downloads mastercomfig-base.vpk from
# the latest GitHub release into tf/custom first if it is missing. Takes effect the next time TF2 starts.
set -u
. "$(dirname "$0")/_env.sh"
CUSTOM="$TF2DIR/tf/custom"
OVR="$TF2DIR/tf/cfg/overrides/modules.cfg"; MARK="// tf2mt balanced preset"
balanced_on() { [ -f "$OVR" ] && grep -qF "$MARK" "$OVR"; }
balanced_off() {   # remove tf2mt's modules.cfg (put the user's own back if we had moved it aside)
  balanced_on && rm -f "$OVR"
  [ -f "$OVR.before-tf2mt" ] && [ ! -f "$OVR" ] && mv "$OVR.before-tf2mt" "$OVR"
  return 0
}
BASE="$CUSTOM/mastercomfig-base.vpk"

hook_file() {   # existing setup_hook.cfg from the comfig app (any custom folder), else the comfig app's location
  local f
  f=$(find "$CUSTOM" -maxdepth 4 -path '*/cfg/app/setup_hook.cfg' 2>/dev/null | head -1)
  echo "${f:-$CUSTOM/comfig-custom/cfg/app/setup_hook.cfg}"
}

case "${1:-status}" in
  status)
    [ -f "$BASE" ] && inst=yes || inst=no
    p=$(grep -hoE '^preset=[a-z-]+' "$(hook_file)" 2>/dev/null | tail -1 | cut -d= -f2)
    [ "${p:-}" = medium ] && balanced_on && p=balanced
    echo "installed=$inst preset=${p:-none}" ;;
  set)
    p=${2:-}
    case "$p" in low|balanced|medium|high|ultra|destitute|custom) ;; *) echo "unknown preset '$p' (low, balanced, medium, high, ultra)"; exit 2 ;; esac
    want=$p; [ "$p" = balanced ] && p=medium
    [ -d "$CUSTOM" ] || { echo "TF2 is not installed yet (no tf/custom)"; exit 1; }
    if [ ! -f "$BASE" ]; then
      url=https://github.com/mastercomfig/mastercomfig/releases/latest/download/mastercomfig-base.vpk
      tmp=$(mktemp)
      if ! curl -fsSL --max-time 60 -o "$tmp" "$url" || [ ! -s "$tmp" ]; then
        rm -f "$tmp"; echo "could not download mastercomfig from GitHub"; exit 1
      fi
      mv "$tmp" "$BASE"; chmod 644 "$BASE"
      echo "installed mastercomfig-base.vpk (latest release)"
    fi
    f=$(hook_file); mkdir -p "$(dirname "$f")"
    if [ -f "$f" ] && grep -q '^preset=' "$f"; then
      sed -i '' -E "s/^preset=[a-z-]+/preset=$p/" "$f"
    else
      echo "preset=$p" >> "$f"
    fi
    if [ "$want" = balanced ]; then
      mkdir -p "$(dirname "$OVR")"
      [ -f "$OVR" ] && ! balanced_on && mv "$OVR" "$OVR.before-tf2mt"
      printf '%s\nshadows=low\nwater=low\npost_processing=off\nanti_aliasing=off\n' "$MARK" > "$OVR"
    else
      balanced_off
    fi
    echo "mastercomfig preset: $want (applies next time TF2 starts)" ;;
  *) echo "usage: $0 status | set <low|balanced|medium|high|ultra>"; exit 2 ;;
esac
