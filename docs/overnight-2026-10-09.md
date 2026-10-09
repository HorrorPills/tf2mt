# Overnight autopilot, 2026-10-09: results

Owner asleep from ~01:05. Everything ran offline on the benchmark demo or local bot matches. Nothing was committed or pushed. The owner's preset (`low`) and config were restored after every test.

## TL;DR
* **New "Balanced" graphics preset** (launcher dropdown, between Low and Medium). It is mastercomfig Medium with its four costliest module groups at Low level: shadows, water, post-processing, anti-aliasing. In a 22-bot match at 120 Hz: **119.2 fps and 52 visible frame jumps/min, against Medium's 114.1 fps and 354 jumps/min**. Medium's lighting, model detail, effects and ragdolls are kept.
* **Preset matrix with bots fighting** (Low → Ultra, vsync and uncapped). Low and Balanced hold 120 Hz. Medium can't quite. High and Ultra are limited to ~60–77 fps by TF2's own CPU work under Rosetta: 2.3× the draw calls of Low. The renderer is not the bottleneck there (encode 2.8 ms, GPU 5.4 ms per frame at High).
* **Bots now fight on Harvest**: generated the missing nav mesh.
* **Crash causes fixed**: test scripts started TF2 before Steam had logged in, and Wine's crash debugger lingered after crashes (`winedbg` is now disabled for tf2mt).
* **Latency experiments that did NOT pan out (removed, documented):** native macOS fullscreen (no direct-to-display gain: 15.5 vs 15.2 ms), CAMetalDisplayLink (+1 refresh of latency), Wine display capture (no effect). The queue drain works on the demo but not in live play, so it is now **opt-in** (`TF2MT_DRAIN=1`).
* Golden frames all pass (SSIM ≥ 0.9991). The final build matches the previous one on the demo. Installed in /Applications/tf2mt.app; nothing committed or pushed.

## 1. Bots now fight on koth_harvest_final
* TF2 ships bot navigation meshes for 46 maps; **koth_harvest_final is not one of them**, which is why bots stood still in spawn. Generated one with `nav_generate` (`tools/bench/gen-nav.sh` → `tf/maps/koth_harvest_final.nav`, 1.6 MB). Bots now play: 91–106 kills per 4-minute run. This also fixes offline bot games on Harvest for the owner.
* New bot benchmark: `tools/bench/night-bots.sh <name> [SECS= VSYNC= WARP= ...]`. It runs 22 bots, first-person spectating that rotates players every 15 s, plus a kill count, and its RCON setup is retried and verified with `status`.

## 2. Crashes found and fixed (test tooling + one real-world cause)
* **TF2 started before Steam has logged in → crash in `engine.dll+0x960b3`.** Since v0.3.1 Steam is shut down after every session, and the test scripts grepped the *whole* connection log for "Logged On", so they found old lines. New helper `ensure_steam` in `scripts/_env.sh` waits for a *new* "Logged On". It is used by tf2mt-smoke, run-census, capture, soak and gen-nav. The launcher's own `play.sh` already did this correctly.
* **Wine's crash debugger leaves processes behind.** When TF2 crashes, `winedbg` + `conhost` stay attached to the dead game. They keep its network port (27015) open, so the next TF2 moves to 27016 (that is how RCON broke) and processes linger. `scripts/_env.sh` now sets `WINEDLLOVERRIDES=winedbg.exe=d` for all tf2mt Wine processes. Steam still writes crash dumps.
* Remaining game-side crashes, not tf2mt: `filesystem_stdio` → `ntdll` write (known), and one `shaderapidx9.dll+0x77796` read in a Medium bot run that did not repeat in 8 later runs.

## 3. Preset matrix (bot match, 22 bots fighting, 4 min, cursor-warp simulation)
| preset | vsync fps | uncapped fps | p99 (vsync) | spikes > 20 ms/min (vsync) | visible jumps/min (vsync) |
|---|---|---|---|---|---|
| Low | 119.9 | 191.3 | 11.6 ms | 0.2 | 8 |
| **Balanced (new)** | **119.2** | — | 12.3 ms | 0.5 | **52** |
| Medium | 114.1 | 131.0 | 13.1 ms | 1.0 | 354 |
| High | 74.7 | 77.1 | 19.0 ms | 11.9 | (below refresh) |
| Ultra | 66.2 | 61.0 | 23.9 ms | 404 | (below refresh) |

Renderer cost per frame (uncapped): Low encode 1.0 / GPU 1.1 ms; Medium 1.5 / 3.0; High 2.8 / 5.4; Ultra 3.5 / 6.2. Draws/frame 411 → 1,063. At High, letting the game run a frame ahead did not help (70 vs 77 fps): TF2's own per-draw CPU work under Rosetta is the limit.

## 4. Where Medium's cost goes (benchmark demo, uncapped; Medium with one group back at Low)
| change from Medium | fps | gain |
|---|---|---|
| Low preset (reference) | 260.7 | |
| Medium preset | 160.7 | |
| shadows → low | 190.0 | **+29** |
| water + post-processing → low/off | 179.4 | **+19** |
| anti-aliasing → off | 173.5 | **+13** |
| lighting / shading / phong → low | 172.3 | +12 |
| LOD / characters → low | 164.7 | +4 |
| decals → off | 163.7 | +3 |
| HUD model / outlines / ropes / texture filter | 162.0 | +1 |
| effects / ragdolls+gibs / sound | ~160 | 0 |
| **Balanced** (shadows, water, post, AA) | **217.8** | +57 |

Implementation: `scripts/mastercomfig.sh set balanced` sets Medium and writes `tf/cfg/overrides/modules.cfg`, marked `// tf2mt balanced preset`. A user's own modules.cfg is moved to `modules.cfg.before-tf2mt` and restored when another preset is chosen. `status` reports `balanced`.

## 5. The "stuck frame" (compositor queue): what was learned
* Per-frame stage log: `TF2MT_LATENCY_CSV=<path>` gives game / drawable wait / encode / submit→screen / total for each frame. The extra refresh is always in submit→screen (15.2 vs 23.5 ms). macOS's compositor holds one frame more.
* Triggers: macOS notifications reliably do it (19.7 % stuck in a demo). In bot matches it appears at random, 0–60 % per run, with no relation to preset, game frame time or synthetic mouse input. In the owner's live session it was the norm (~31 ms).
* Queue drain (skip one present + one refresh pause): every drain fixes it instantly on the demo, and stuck time drops 19.7 % → 1.9 % with notifications. But live, it fired every ~7 s and latency stayed ~31 ms. Each drain is a repeated frame, so it is **opt-in** now (`TF2MT_DRAIN=1`).
* Tried and removed: native fullscreen (Wine leaves it on mode reset; once held, submit→screen 15.5 ms, so no direct-to-display gain), CAMetalDisplayLink (perfect pacing, sd 0.07 ms, but +1 refresh: 33 ms), Wine `CaptureDisplaysForFullscreen` (no effect on a borderless window).
* Open: what makes live play stick. It needs a live session with `TF2MT_LATENCY_CSV` set, to see which stage grows during real play.

## Files changed tonight (uncommitted)
`scripts/_env.sh` (ensure_steam, winedbg off), `scripts/mastercomfig.sh` (balanced), `tools/launcher/TF2Launcher.swift` (Balanced in the dropdown), `src/unixlib/render.mm` (latency CSV, opt-in drain), `src/unixlib/tf2mt_unix.m` (display-link and fullscreen experiments removed), `tools/census/*`, `tools/replay/*` (ensure_steam), new `tools/bench/{gen-nav,night-run,night-bots,night-presets,night-modules,night-notif,night-summary,cursorwarp}`, docs. Raw data: `~/Games/tf2/logs/night/`.
