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

**Fix:** `tools/wine-patches/winemac_warp_nodiscard.py apply|revert|status` binary-patches our private runtime's `x86_64-unix/winemac.so`. It NOPs the `lastSetCursorPositionTime` store and sets the discard mask to 0. The script verifies the original bytes first, re-signs ad hoc and swaps the file in atomically. Backup: `~/Games/tf2/backups/winemac.so.orig`. Trade-off: apps that rely on absolute cursor positions immediately after a warp may see stale positions (pointer jitter). TF2 uses raw deltas (`m_rawinput 1`) and is unaffected. **A runtime update overwrites the patch:** re-run `apply` (it refuses on an unknown build).

**Presentation default changed** to `d3d9.tearFree = True` (`~/Games/tf2/config/dxvk.conf`). MoltenVK has no MAILBOX, so this is FIFO with 3 images: no tearing, owner-verified smooth. The old immediate-mode config is kept as `dxvk-immediate.conf`.

Side note: TF2 crashed once on exit in Source's async file thread (`filesystem_stdio` → `kernelbase` → `ntdll` write past a buffer). Backtrace in `~/Games/tf2/logs/runs/mouse-norawinput/backtrace-exit-crash.txt`. Not investigated yet.
