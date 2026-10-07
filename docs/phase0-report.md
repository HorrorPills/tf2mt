# Phase 0 report — baseline, ceiling, go/no-go

Status 2026-10-07: **G0 PASSES. Proceed to P1** (narrowly on condition 2; see §4). The deciding reruns (§2b) were taken with the display awake and kept awake by `caffeinate`.

All numbers below come from runs with the display awake. The raw data for every run is in `~/Games/tf2/logs/runs/<tag>/`: frames CSV, per-thread CPU CSV, console log, args and report.

---------------------------------------------------------------------------
## 1. Method

| Item | Value |
|---|---|
| Machine | M1 Max 10C/24G, 32 GB, macOS 27.0.1, panel 1920×1080 @ **120 Hz** |
| Stack | Wine 10.0 (Sikarugir, x86_64 under Rosetta 2) → DXVK 2.4.1 (async-patched build) → MoltenVK 1.4.1 → Metal |
| TF2 | 64-bit `tf_win64.exe`, buildid 25738890; 1920×1080 borderless (`-windowed -noborder`); owner launch options |
| Game config | Owner's real config: **mastercomfig preset `low`** (`tf/custom/comfig-custom/cfg/app/setup_hook.cfg`; PLAN.md says `medium`, which is wrong) with the owner's addons |
| Scenario | `tf/bench.dem`: 206 s on `koth_harvest_final`, recorded on a local listen server with 23 bots (`tf_bot_difficulty 2`). The recorder was a first-person spectator (`spec_mode 4`) cycling `spec_next` every 15 s. Played back in real time (`playdemo`, not `timedemo`) with `demo_quitafterplayback 1`. One fresh process per run (F12). |
| Window | From demo start (end of the map-load stall) + 10 s, to the last in-game frame − 2 s: ≈195 s and 23k–110k frames per run |

**Instruments built for this phase** (all from scratch, x86_64 PE unless noted):

* `tools/trace` (`d3d9.dll`): timing proxy. Generic forwarding thunks are generated from mingw-w64 `d3d9.h` (`gen_methods.py`), with per-method call counters. It writes one CSV row per `Present` (interval, time in Present, draws, calls), names each Present-calling thread `d3d9-present-N` so the macOS side can identify it, and dumps call counts at exit. It forwards to `d3d9_ref.dll`, which is either the runtime DXVK or the null renderer.
* `tools/null` (`d3d9.dll`): throwaway null renderer. The device accepts every call and draws nothing; `Lock` returns real, lazily committed memory; queries complete immediately. `IDirect3D9` forwards to the DXVK oracle (black box), so adapter identity, caps and format answers, and therefore Source's dxlevel and render paths, are identical to the DXVK runs. The call counts confirm it: identical draws/frame (p50 248 vs 256 incl. Present-only frames; max 415 vs 421). It hit **zero** unimplemented methods during a full demo.
* `tools/bench/threadmon` (native arm64): per-thread CPU sampler via `proc_pidinfo`, 1 Hz.
* `tools/bench/analyze.py`: percentiles, 1%/0.1% lows, spikes, hitches/min, Present time, per-thread CPU. Ported from `legacy_parallels_analyze.py`.
* `tools/bench/hitches.py`: per-hitch list with cross-run alignment on demo time. A hitch present in the null run at the same demo time is game-side.
* `tools/bench/rcon.py`: Source RCON client. A listen server with `-usercon +rcon_password` gives scriptable console control without touching game memory. Used to record the demo.
* `tools/bench/displaystate` (native): validity guard (display awake and session unlocked).
* `scripts/bench.sh <tag> <dxvk|null> [args]`: one complete run.

---------------------------------------------------------------------------
## 2. Results (display awake, valid)

| Run | Config | avg fps | p50 | p95 | p99 | p99.9 | 1% low | hitch >20 ms /min | in-Present p50 / p99 |
|---|---|---|---|---|---|---|---|---|---|
| `p0fp-null-fps0` | **null renderer**, `fps_max 0` | **569.9** | **1.67** | 2.46 | **3.79** | 10.02 | 116.1 | 2.16 | 0 / 0 |
| `p0fp-dxvk-fps0` | DXVK default, `fps_max 0` | 256.1 | 3.36 | 8.17 | 11.47 | 14.38 | 74.8 | 2.77 | **1.47 / 8.25** |
| `p0t-null-fps120` | null, `fps_max 120` | 119.2 | 8.35 | 9.91 | 15.95 | 19.67 | 54.5 | 6.77 | 0 / 0 |
| `p0fp-dxvk-fps120` | DXVK default, `fps_max 120` | 118.9 | 8.36 | 10.03 | 15.95 | 19.90 | 53.2 | 7.08 | 0.03 / 0.11 |
| `p0t-async-cold-fps120` | DXVK `enableAsync` + state cache (cold), 120 | 118.8 | 8.36 | 10.04 | 15.88 | 18.37 | 52.5 | 5.87 | 0.03 / 0.10 |
| `p0t-async-warm-fps120` | same, warm cache, 120 | 119.0 | 8.36 | 10.09 | 15.94 | 18.46 | 55.8 | 3.70 | 0.03 / 0.10 |
| `p0t-async-lat1-fps120` | + `d3d9.maxFrameLatency=1`, 120 | 118.6 | 8.36 | 10.37 | 16.26 | 20.23 | 51.4 | 8.01 | 0.03 / 0.51 |

### 2b. Reruns, 2026-10-07 (display awake; these decide condition 2)

| Run | Config | avg fps | p50 | p95 | p99 | p99.9 | 1% low | hitch >20 ms /min | in-Present p50 / p99 |
|---|---|---|---|---|---|---|---|---|---|
| `p0r-dxvk-default-fps0` | DXVK default, `fps_max 0` | 220.2 | 4.37 | 8.21 | 11.71 | 15.10 | 70.8 | 3.08 | 2.23 / 8.86 |
| `p0r-cacheonly-fps0` | state cache only | 236.4 | 3.94 | 7.72 | 11.55 | 14.87 | 74.1 | 3.39 | 1.90 / 8.26 |
| `p0r-async-fps0` | `enableAsync` + state cache | 236.6 | 3.83 | 8.10 | 11.67 | 14.37 | 74.8 | 1.54 | 1.86 / 8.30 |
| **`p0r-async-fps400`** | same, `fps_max 400` (owner's usual cap) — **best tuned DXVK** | **272.4** | 3.09 | 6.51 | **10.46** | 13.63 | 81.4 | **0.92** | 0.81 / 5.65 |
| `p0r-async-vsync` | async + `d3d9.presentInterval=1`, `fps_max 0` | 119.8 | 8.34 | 9.80 | 14.03 | 17.30 | 60.9 | 2.16 | 5.74 / 7.41 |
| `p0r-msync-null-fps0` | null, `WINEMSYNC=1` session | 577.9 | 1.64 | 2.39 | 3.37 | 11.82 | 97.2 | 3.08 | 0 / 0 |
| `p0r-msync-async-fps0` | async, `WINEMSYNC=1` session | 194.9 | 4.80 | 10.83 | 12.94 | 17.11 | 63.5 | 6.76 | 2.61 / 9.91 |

Findings from the reruns:
* **The overnight hypothesis is refuted.** Async does *not* remove DXVK's in-`Present` blocking (1.86 ms p50 vs 2.23 default). Async's real gain is fewer compile hitches (1.54 vs 3.39/min with cache only).
* **msync hurts DXVK** (195 fps, 6.8 hitches/min) and doesn't help the null renderer. Don't use it.
* With the display awake, `presentInterval=1` really is vsync (Present blocks ~5.7 ms, 119.8 fps). p99 is still 14 ms, because any frame > 8.33 ms costs a whole extra vblank.
* `fps_max 400` beats uncapped (272 vs 237 fps, p99 10.5 vs 11.7). Less time is lost to `Present` back-pressure.

(Frame times in ms; "fps" means presents per second. Earlier runs on a 26 s fragment and on a demo with a static camera are kept in `runs/` but superseded.)

---------------------------------------------------------------------------
## 3. Attribution (P0.5)

### 3.1 Frame time (uncapped)
* **Game-only cost (null renderer): p50 1.67 ms, p99 3.79 ms.** This refutes PLAN.md F10 and §3 ("TF2's own work ≈ 7 ms/frame"). At the owner's preset on this machine, TF2 under Rosetta has about 5× headroom for 120 fps. The main risk in §3 and §12 ("game-only cost already > 8.3 ms") is **retired**.
* DXVK default adds **+1.69 ms at p50 (50% of the frame)** and +7.7 ms at p99. The largest single item is **time blocked inside `Present`: 1.47 ms p50, 8.25 ms p99**. This is DXVK/MoltenVK presentation in `IMMEDIATE` mode with 2 swapchain images.

### 3.2 CPU per frame by thread (uncapped; ms of CPU per presented frame)
| Thread | null | DXVK default | Δ |
|---|---|---|---|
| Source main thread (`d3d9-present-1`) | 1.57 | 2.41 | +0.84 |
| Source render thread (`d3d9-present-2`, issues D3D9 calls from frame 8 on) | 0.48 | 1.07 | **+0.59 (55% of render-thread time is translation)** |
| `dxvk-submit` | — | 0.84 | +0.84 |
| `dxvk-cs` | — | 0.57 | +0.57 |
| 4 unnamed workers (MoltenVK/Metal/Wine) | 0.21 | 0.88 | +0.67 |
| **Total** | **2.29** | **5.77** | **+3.5 ms CPU/frame (60%)** |

Open question for P1: why does the **main** thread get +0.84 ms/frame under DXVK? The census will show per-thread call counts (dynamic `Lock`/`Unlock`, resource creation, or queued-mode sync).

MoltenVK self-reports (performance tracking, from a run later invalidated by display sleep; these counts don't depend on presentation): **1,584 SPIR-V→MSL conversions per session (avg 2.1 ms, max 17 ms)**, plus 831 pipeline compiles. MoltenVK's pipeline cache is never read or written (0 hits), so all of this repeats every launch; only DXVK's state cache persists.

### 3.3 Hitch taxonomy (uncapped, > 20 ms, aligned on demo time)
DXVK default had 9 hitch frames in the window:
* **Renderer-only, 6 frames in 3 events** (the null run is clean at the same moment): t=29.6 s (25 ms); **t=91.7–92.0 s burst of 4 frames (27/48/62/53 ms)**; t=158.9 s (22 ms). The pattern (short bursts when new content appears) fits pipeline/shader compilation. Confirming this per hitch needs the P1 trace (`Create*Shader`/first-use timestamps).
* **Game, 3 frames in 2 events** (the null run hitches too): t=30.4 s (~190–200 ms in both), t=45.4 s. They land on the recorder's 15 s `spec_next` switches: spectator-target loading, a recording artifact that normal play doesn't have.

**At `fps_max 120`, the null renderer is just as jittery as DXVK** (p99 15.95 ms, 6.8 hitches/min). That jitter is Source's own frame limiter under Wine/macOS (occasional sleep overshoot → doubled frame), not the renderer. Consequences:
1. At `fps_max 120`, p99 and hitch counts are dominated by the limiter: 3.7–8.0/min run to run for the *same* renderer. They can't rank renderers.
2. The DoD target "presented-interval stdev ≤ 0.5 ms" can't be reached with `fps_max` pacing at all, whatever the renderer. It requires renderer-side pacing (PLAN D6: vblank/display-link driven `Present`, `fps_max 0`). That is a real argument *for* an own backend, provided it does the pacing.

---------------------------------------------------------------------------
## 4. Gate G0 — PASS

| Condition | Result | Status |
|---|---|---|
| 1. Null game-only p99 ≤ 6.0 ms | **3.37–3.79 ms** | **PASS** |
| 2. Tuned DXVK is *not* already ≥ 115 fps, p99 ≤ 10 ms, ≤ 3 hitches/min | Best tuned DXVK (`async`, `fps_max 400`): 272 fps ✓, 0.92 hitches/min ✓, **p99 10.46 ms ✗**. Every other tuned variant: p99 11.6–14.0 ms. | **PASS (narrow)** |
| 3. Translation ≥ 25% of render-thread time, or ≥ 50% of hitches are compile | 55% of render-thread CPU; DXVK default p50 4.37 ms vs null 1.64 ms → **62% of frame time**; in-`Present` blocking alone is 1.9–2.2 ms/frame | **PASS** |

**Decision: continue to P1 (trace & census).** Notes for the owner:
* The margin on condition 2 is small (10.46 vs 10 ms). Tuned DXVK is already a big improvement for everyday play. **Applied as the default** (2026-10-07): `~/Games/tf2/config/dxvk.conf` (`dxvk.enableAsync = True`, `dxvk.enableStateCache = True`) via `scripts/_tuned.sh`, sourced by `steam.sh` and `tf2.sh`; `fps_max 400` (already in the owner's config); **no msync**. `bench.sh` opts out with `TF2_TUNED=0`.
* The strongest case for tf2mt is structural, not average fps: (a) ~2 ms/frame of `Present` back-pressure in the DXVK→MoltenVK path, and (b) per-session SPIR-V→MSL recompiles (MoltenVK pipeline cache unused). Also, (c) even pacing at 120 Hz is impossible through `fps_max` (null renderer has the same limiter jitter), so it needs renderer-side display-link pacing (D6) that also tolerates frames > 8.33 ms without a full-vblank penalty.

---------------------------------------------------------------------------
## 5. Invalid runs (display asleep), quarantined

The display slept at **00:14:41** (10-minute idle display sleep) and the screen locked. Runs from 00:15 onward are in `~/Games/tf2/logs/runs-invalid-display-off/`. With nothing composited they showed a fake 465–500 fps; the reruns in §2b replace them. `scripts/bench.sh` now refuses to start on a locked or asleep screen, keeps the display awake with `caffeinate -d`, and writes `INVALID` if the state changes mid-run. For unattended sessions: `caffeinate -dimsu -t 86400 &` plus `defaults -currentHost write com.apple.screensaver idleTime -int 0` (the screensaver idle time was unset before; restore with `defaults -currentHost delete com.apple.screensaver idleTime`).

---------------------------------------------------------------------------
## 7. Facts learned (corrections and additions to PLAN.md §2)

* **F10 is wrong for the current setup:** game-only ≈ 1.7 ms/frame p50 (preset `low`, this demo), not ≈ 7 ms.
* **F11 resolved:** an x86_64 PE `d3d9.dll` beside `tf_win64.exe` loads and wins (AppDefaults override `d3d9=native` for `tf_win64.exe`). The trace proxy works this way with no Wine changes.
* TF2 calls **`Direct3DCreate9Ex`** but then plain **`CreateDevice`** (not `CreateDeviceEx`). Behavior flags `0x56` = `FPU_PRESERVE | MULTITHREADED | PUREDEVICE | HARDWARE_VERTEXPROCESSING`. Present params: A8R8G8B8 1920×1080, 1 back buffer, `SWAPEFFECT_DISCARD`, windowed, auto depth `D24S8`, `PRESENTATION_INTERVAL_IMMEDIATE`. Adapter as reported by DXVK: "Apple M1 Max", vendor 0x106b.
* A Source **render thread** issues the D3D9 calls in-game (it presents from frame 8 on); the main thread presents during startup and loading only.
* Per-frame D3D9 traffic in this demo: about 250 draws (max ~420), 1,550–1,650 device calls/frame. Per frame, `SetStreamSource` ≈330 > `SetVertexShaderConstantF` ≈305 > `DrawIndexedPrimitive` ≈245 > `SetTexture` ≈215 > `SetPixelShaderConstantF` ≈113 > `SetRenderState` ≈92 dominate. **Only `DrawIndexedPrimitive` is used for drawing** (no `DrawPrimitive`/`*UP` calls in a full demo).
* Valve ships `bin/x64/dxvk_d3d9.dll` (for `-vulkan`), a further oracle candidate.
* Wine's `GetThreadTimes` returns process-level, 10 ms-quantised values, so it can't be used for thread CPU. Use `threadmon`.
* The runtime has **msync** (`WINEMSYNC=1`), not usable esync. It must be enabled for wineserver, Steam and the game together.
* **Launch gotchas:** (a) TF2 started before Steam has logged on runs `-insecure` and crashes during load: wait for `Logged On` in `Steam/logs/connection_log.txt`. (b) `/usr/bin/nohup` is SIP-protected and strips `DYLD_*`, which caused the FreeType warning; removed from `steam.sh`/`tf2.sh`. (c) A local listen server needs `sv_lan 1`, or the client is kicked after ~1 min ("Invalid STEAM UserID Ticket"). (d) `fps_max` can only be set at launch, not while connected.
* DXVK's state cache lives where **Steam** points it: `steamapps/shadercache/440/DXVK_state_cache/tf_win64.dxvk-cache` (2,162 entries). Steam overrides `DXVK_STATE_CACHE_PATH` for games it launches, so the `~/Games/tf2/cache/dxvk/state` path used in the P0 runs was never in effect. The `tf_win64.dxvk-cache` beside the exe (956 bytes) is a stale leftover.
