# Mouse turning felt like 30–40 fps (fixed 2026-10-07)

**Symptom (owner):** at a steady 120 fps (Metal HUD), turning with the mouse felt like 30–40 fps. Walking with WASD was perfectly smooth.

**Measurement:** `tools/trace` hashes the view-projection matrix (VS constants c8–c11) on each frame, giving camera updates per second against frames per second (`tools/bench/mouse-test.sh` + `mouse-report.py`).

| run | frames/s | camera updates/s while turning |
|---|---|---|
| baseline (vsync) | 120 | **~40** (strafing: 120) |
| `UseConfinementCursorClipping=n` | 120 | 40 |
| `m_rawinput 0` | 120 | 40 |
| uncapped/immediate | 120 while moving* | 27–40 |
| **winemac patch** | 120 | **~117–120** |

\* With immediate presentation, `Present` blocked ~6 ms per frame (8.33 ms cadence) whenever the owner moved. That's a separate Wine/macOS presentation effect; not pursued further now that vsync (tearFree) is the default.

**Ruled out:** macOS delivers 240 mouse events/s (native test window), Wine's driver polling (an 8 kHz throttle in `win32u`), and the Cocoa main thread (idle ~71% while turning).

**Root cause:** TF2 recentres the cursor (`SetCursorPos`) every frame. In Wine's Mac driver, `-[WineApplicationController setCursorPosition:]` (`dlls/winemac.drv/cocoa_app.m`) then (1) discards all queued mouse-move events and (2) ignores every event timestamped before the warp, forcing the next one to be absolute. At 120 warps/s most motion is thrown away.

**Fix:** `tools/wine-patches/winemac_warp_nodiscard.sh apply|revert|status` binary-patches our private runtime's `x86_64-unix/winemac.so`. It NOPs the `lastSetCursorPositionTime` store and sets the discard mask to 0. The script verifies the original bytes first, re-signs ad hoc and swaps the file in atomically. Backup: `~/Games/tf2/backups/winemac.so.orig`. Trade-off: apps that rely on absolute cursor positions immediately after a warp may see stale positions (pointer jitter). TF2 uses raw deltas (`m_rawinput 1`) and is unaffected. **A runtime update overwrites the patch:** re-run `apply` (it refuses on an unknown build).

**Presentation default changed** to `d3d9.tearFree = True` (`~/Games/tf2/config/dxvk.conf`). MoltenVK has no MAILBOX, so this is FIFO with 3 images: no tearing, owner-verified smooth. The old immediate-mode config is kept as `dxvk-immediate.conf`.

Side note: TF2 crashed once on exit in Source's async file thread (`filesystem_stdio` → `kernelbase` → `ntdll` write past a buffer). Backtrace in `~/Games/tf2/logs/runs/mouse-norawinput/backtrace-exit-crash.txt`. Not investigated yet.

# Aim felt sluggish at a steady 120 fps (2026-10-08, Metal renderer)

**Symptom (owner, public match, vsync 120 Hz):** steady 120 fps, but aiming felt sluggish, not on par with the refresh rate.

**Causes and fixes:**
1. **`m_filter 1`** came from a local test override (`tf/cfg/overrides/autoexec.cfg`). It averages the mouse delta over two frames, so it adds lag and a floaty feel. The owner set `m_filter 0` in the console and felt "much smoother". The override line was removed. With the filter off, game-side frame-time spikes now show as single-frame camera jumps (live: ~55 frames/min over 12.5 ms, all game work; the time inside tf2mt was ~0.01 ms). The same spikes occur on DXVK.
2. **Deep frame queue.** New instrumentation (unix log `latency:` lines) measures the time from the moment the game may start frame *k* to macOS `presentedTime` of frame *k*. It also counts shown intervals over 12.5 ms, measured at the display. Bench demo, vsync, `tools/bench/latency-ab.sh` + `latency-summary.py`:

| pacing (game ahead / drawables / in flight) | frame start → on screen p50 | visible jumps/min |
|---|---|---|
| 1 / 3 / 2 (old default) | 40.7 ms (bistable 37.7 ↔ 45.6) | 3.4 |
| 0 / 3 / 2 | 36.3 ms | 5.9 |
| 1 / 2 / 2 | 31.9 ms | 6.2 |
| **0 / 2 / 2 (new default)** | **25.1 ms** (repeat run: 30.7 ms) | 6.8 (repeat: 4.6) |
| 0 / 2 / 1 | 25.7 ms | 2.8 |
| 2 / 3 / 2 | 43.5 ms | 4.3 |

New defaults are game ahead 0 (`TF2MT_GAME_AHEAD`, render.mm) and 2 drawables (`TF2MT_DRAWABLES`, tf2mt_unix.m). In flight stays at 2 (`TF2MT_INFLIGHT`, or the older `TF2MT_MAX_LATENCY`). All three env vars remain overrides.

**Open:** the queue depth is bistable. After a hitch it can keep one extra frame (+8 ms), because the game runs at the display rate and the queue never drains. The next step is a drain/pacing controller that briefly delays the game's frame start when measured latency exceeds the target. Another candidate is Reflex-style just-in-time frame starts. Online jump rate with the new defaults still has to be checked: an earlier online test of a short queue, with `m_filter 1` still on, showed more missed refreshes.
