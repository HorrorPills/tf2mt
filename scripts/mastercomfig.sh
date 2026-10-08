#!/bin/bash
# mastercomfig graphics presets (https://github.com/mastercomfig/mastercomfig), used by the launcher.
#   scripts/mastercomfig.sh status          -> "installed=<yes|no> preset=<name|none>"
#   scripts/mastercomfig.sh set <preset>    -> low | medium | high | ultra (also: destitute, custom)
# mastercomfig 9.x selects its preset from a `preset=<name>` line in cfg/app/setup_hook.cfg (the comfig app's
# file). `set` rewrites only that line (addons and other settings stay), and downloads mastercomfig-base.vpk from
# the latest GitHub release into tf/custom first if it is missing. Takes effect the next time TF2 starts.
set -u
. "$(dirname "$0")/_env.sh"
CUSTOM="$TF2DIR/tf/custom"
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
    echo "installed=$inst preset=${p:-none}" ;;
  set)
    p=${2:-}
    case "$p" in low|medium|high|ultra|destitute|custom) ;; *) echo "unknown preset '$p' (low, medium, high, ultra)"; exit 2 ;; esac
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
    echo "mastercomfig preset: $p (applies next time TF2 starts)" ;;
  *) echo "usage: $0 status | set <low|medium|high|ultra>"; exit 2 ;;
esac
