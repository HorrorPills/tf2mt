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

# The "stuck frame": compositor queue drain (2026-10-09)

**Problem:** with vsync, frame start → on screen flips between ~23.5 ms and ~31.8 ms and stays at one level for 10–70 s (`docs/mouse-input.md` above; ~41/31 ms before the pacing change). A per-frame stage log (`TF2MT_LATENCY_CSV=<path>` → `frame,start,game_ms,drawable_wait_ms,encode_ms,to_screen_ms,total_ms`) shows that the game, the drawable wait and the encode are the same in both states. The whole extra refresh is in **submit → on screen** (15.2 vs 23.5 ms at 120 Hz). macOS's compositor sometimes holds one extra frame, and because frames arrive exactly once per refresh, that queue never drains.
* **Getting stuck** is spontaneous on the macOS side: no game or renderer event at the switch. On-screen activity triggers it. With a notification every 15 s, 19.7 % of frames were stuck, against 0 % in quiet runs.
* **Getting unstuck** happened only after a real gap of a full refresh with no submission, such as a game hitch of ≥ 14 ms. A short hiccup followed by a quick frame refilled the queue.

**Fix (render.mm, opt-in since the overnight test: `TF2MT_DRAIN=1`; vsync only):** the presented handler collects submit→screen times per half second and takes the median. The floor is the lowest median of the last 5 minutes. Two consecutive half-seconds above floor + ½ refresh request a drain. The encoder then skips presenting one frame and waits one refresh, then continues. Drains are at least 2 s apart. One probe drain runs ~15 s after the first frame (loading/menu, invisible) so the floor is learned even if a session starts stuck. Log line: `drain: skipped presenting one frame (probe|queue stuck, …)`.

| bench demo, vsync 120 Hz, notification every 15 s | stuck frames | mean frame start → screen | drains |
|---|---|---|---|
| no drain | 19.7 % | 25.3 ms | — |
| first version (1 s windows, 5 s apart, no probe) | 14.6 % | 24.9 ms | 9 (each fixed the queue instantly) |
| **final** | **1.9 %** | **23.8 ms** | 5 |
| final, quiet run (no notifications) | 0 % | 23.6 ms | 1 (the probe only, no false drains) |

Cost: each drain is one repeated frame, the same thing a stuck queue would cause on its own. Untested at 60 Hz: the detection is relative to the refresh interval, so it should carry over.

**Live result (owner session 2026-10-09 00:48):** 141 drains in 16 min, and frame start → screen stayed ~29–31 ms most of the time. In live play the queue re-sticks within seconds, so the drain only adds repeated frames. It is off by default; see docs/overnight-2026-10-09.md §5 for what else was tried.

# Metal vs DXVK motion smoothness (2026-10-09)

**Owner report:** on Metal the aim felt less continuous than on DXVK, with tiny skips and the view sometimes "jumping ahead", only while moving the mouse.

* **Input is equally smooth on both paths.** Camera yaw is logged per frame from the view-projection matrix (c11 row = camera forward): `cam_yaw` / `cam_pitch` in the Metal latency log and in the DXVK trace frame log (`tools/bench/camera-smoothness.py`). In live sessions with similar mouse use, irregular turning frames were 33.8 % on Metal vs 36.2 % on DXVK (mostly mouse-count quantization: 0.154° per count at sensitivity 7).
* **The difference is presentation.** With "game ahead 0", every TF2 frame-time spike changed the mouse→screen delay from one frame to the next (112 changes > 4 ms per minute live) and became a visible repeat. DXVK's deep queue hides that, at ~41 ms latency.
* **Balanced bot match at 120 Hz:**

| game ahead / drawables | visible repeats/min | motion jolts/min | latency p50 |
|---|---|---|---|
| 0 / 2 (previous default) | 50.9 | 64.6 | 23.5 ms |
| **1 / 2 (new default)** | **24.6** | **35.7** | **27.4 ms** |
| 0 / 3 | 35.2 | 56.5 | 31.8 ms |
| 1 / 3 (old default, DXVK-like) | 29.4 | 48.6 | 43.1 ms |

Tried as the default, then **reverted the same day**: the owner felt the extra frame as the aim dragging behind the hand ("slippery"), which was worse than the spike repeats. The default stays `TF2MT_GAME_AHEAD=0`.
