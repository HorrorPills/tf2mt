# Behaviour checks for relative mouse mode (issue #2, src/unixlib/relmouse.m). Sourced by tools/bench/mouse-fps.sh
# with the game in first-person play:  tools/bench/mouse-fps.sh <name> CHECK=tools/bench/relmouse-checks.sh [RENDERER=..]
# Mouse input comes from build/tools/hiddrive (HID relative motion: moves the cursor only while the mouse is associated,
# like a real mouse). Results: $out/checks.txt (PASS/FAIL lines). CHECKS="..." picks a subset; KILL=1 ends the game
# with kill -9 instead of quit (crash path).
RLOG="$TF2_HOME/logs/relmouse.log"
pass=0; fail=0
result() { if [ "$1" = ok ]; then pass=$((pass + 1)); echo "PASS $2" >> "$out/checks.txt"; else fail=$((fail + 1)); echo "FAIL $2 (${3:-})" >> "$out/checks.txt"; fi; }
verdict() { [ "$1" = 1 ] && echo ok || echo no; }
pos() { build/tools/mousedrive pos; }
hid() { build/tools/hiddrive 500 "${2:-4}" "on:$1" "${3:-0}" >/dev/null; }
shot() { screencapture -x "$S/$1.png"; }
turned() {   # $1 = seconds of motion; true if the picture changed a lot (the camera turned)
  shot t0; hid "$1" "${2:-7}" "${3:-0}"; sleep 0.3; shot t1   # 0.3 s at 7 counts x 500/s ~ 170 degrees
  local d; d=$(build/tools/imgdiff "$S/t0.png" "$S/t1.png"); echo "  turn diff $d" >> "$out/checks.txt"
  awk -v d="$d" 'BEGIN { exit !(d > 0.12) }'   # idle frames differ by ~0.02, a 170-degree turn by >= 0.16
}
cursor_moves() {   # true if HID motion moves the macOS cursor (right, then left: one of them is away from an edge)
  local a b c; a=$(pos); hid 0.2 -6; sleep 0.2; b=$(pos); hid 0.2 6; sleep 0.2; c=$(pos)
  echo "  cursor $a -> $b -> $c" >> "$out/checks.txt"; [ "$a" != "$b" ] || [ "$b" != "$c" ]
}
parked() {   # true if HID motion leaves the macOS cursor where it is
  local a b c; hid 0.1 4; sleep 0.2; a=$(pos); hid 0.3 -6; sleep 0.2; b=$(pos); hid 0.3 6; sleep 0.2; c=$(pos)
  echo "  cursor $a -> $b -> $c" >> "$out/checks.txt"; [ "$a" = "$b" ] && [ "$b" = "$c" ]
}
log_has() { grep -q "$1" "$RLOG" 2>/dev/null; }
log_count() { grep -c "$1" "$RLOG" 2>/dev/null || echo 0; }
activate_game() {
  local p; p=$(tf2_pids | head -1)
  osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $p) to true" >/dev/null 2>&1
}
want() { [ -z "${CHECKS:-}" ] || [[ " $CHECKS " == *" $1 "* ]]; }
: > "$out/checks.txt"
echo "renderer $RENDERER, display $(build/tools/refresh get) Hz" >> "$out/checks.txt"

# 1. in game: relative mode on, cursor parked, camera turns
if want ingame; then
  ok=1
  turned 0.3 || ok=0
  parked || ok=0
  log_has "relmouse: on" || ok=0
  result "$(verdict $ok)" "in game: relative mode on, cursor parked, camera turns" "see lines above"
fi

# 2. vertical motion turns the camera too
if want vertical; then
  turned 0.3 0 4 && result ok "vertical motion turns the camera" || result no "vertical motion turns the camera" "no picture change"
fi

# 3. shooting while turning (left button held: drag events)
if want drag; then
  build/tools/mousedrive down; ok=1; turned 0.3 || ok=0; build/tools/mousedrive up; sleep 0.5
  result "$(verdict $ok)" "turning with the left button held (drag events)" "no picture change"
fi

# 4. menu: Escape shows the cursor -> mode off, cursor follows the mouse; Escape again -> back in relative mode
if want menu; then
  n_on=$(log_count "relmouse: on")
  build/tools/mousedrive key 53; sleep 2
  ok=1; cursor_moves || ok=0; log_has "relmouse: off (cursor shown" || ok=0
  result "$(verdict $ok)" "menu open: mode off, cursor follows the mouse" "cursor still or no 'off' line"
  build/tools/mousedrive key 53; sleep 2
  ok=1; turned 0.3 || ok=0; [ "$(log_count "relmouse: on")" -gt "$n_on" ] || ok=0
  parked || ok=0
  result "$(verdict $ok)" "menu closed: relative mode again, cursor parked, camera turns" "see lines above"
fi

# 5. app switch (Cmd-Tab): the mouse must work in other apps at once; back in game -> relative again
if want appswitch; then
  osascript -e 'tell application "Finder" to activate' >/dev/null 2>&1; sleep 2
  ok=1; cursor_moves || ok=0; log_has "relmouse: off (app inactive" || log_has "relmouse: off (cursor shown or app inactive" || ok=0
  result "$(verdict $ok)" "other app in front: cursor follows the mouse" "cursor still or no 'off' line"
  activate_game; sleep 2
  build/tools/mousedrive click 960 540; sleep 1   # focus click into the game (fires a rocket)
  ok=1; turned 0.3 || ok=0; parked || ok=0
  result "$(verdict $ok)" "back in game after app switch: relative mode, camera turns" "see lines above"
fi

# 6. m_rawinput 0 (cursor-position input): camera turns, and no drift when the mouse stops
if want rawinput0; then
  rcon 'm_rawinput 0'; sleep 1
  ok=1; turned 0.3 || ok=0
  sleep 1; shot i0; sleep 2; shot i1
  d=$(build/tools/imgdiff "$S/i0.png" "$S/i1.png"); echo "  idle diff $d" >> "$out/checks.txt"
  awk -v d="$d" 'BEGIN { exit !(d < 0.05) }' || ok=0
  rcon 'm_rawinput 1'; sleep 1
  result "$(verdict $ok)" "m_rawinput 0: camera turns, no drift when idle" "see lines above"
fi

# 7. the game ends (quit, or kill -9 with KILL=1): the mouse moves the cursor again
if want exit; then
  if [ "${KILL:-0}" = 1 ]; then for p in $(tf2_pids); do kill -9 "$p"; done; how="killed (kill -9)"
  else rcon quit; how="quit"; fi
  for _ in $(seq 30); do tf2_running || break; sleep 1; done
  sleep 2
  cursor_moves && result ok "game $how: cursor follows the mouse" || result no "game $how: cursor follows the mouse" "cursor still"
fi
echo "checks: $pass passed, $fail failed" >> "$out/checks.txt"
