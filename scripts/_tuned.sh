# Tuned defaults for normal play (docs/phase0-report.md §4, docs/loadout-stall.md).
# Sourced by steam.sh and tf2.sh, so games started from the Steam UI inherit it too.
# Opt out with TF2_TUNED=0 (scripts/bench.sh does this so stock-DXVK baselines stay stock).
#  - DXVK async pipeline compile + state cache (fewer compile hitches) + tearFree (vsync, 3 buffers).
# Deliberately NOT set: WINEMSYNC (costs ~20% fps: 272 -> 216 in the demo, and does NOT fix the loadout
#   stalls — see docs/loadout-stall.md), fps_max (owner config already has 400; never cap at 120).
if [ "${TF2_TUNED:-1}" = 1 ]; then
  export DXVK_CONFIG_FILE="${DXVK_CONFIG_FILE:-$TF2_HOME/config/dxvk.conf}"
fi

# msync must match the running wineserver (Steam's session), otherwise the game exits silently.
# steam.sh records the session's mode; launching into an existing session follows it.
TF2_SESSION_SYNC="$TF2_HOME/cache/wine-session-sync"
follow_session_sync() {
  if pgrep -f "$TF2_HOME/wine/.*wineserver" >/dev/null && [ -f "$TF2_SESSION_SYNC" ]; then
    if [ "$(cat "$TF2_SESSION_SYNC")" = msync ]; then export WINEMSYNC=1; else unset WINEMSYNC; fi
  fi
}
