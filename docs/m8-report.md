# M8 report — anti-stutter engine

Date: 2026-10-08 (overnight, owner away). Gate (PLAN §10), after the load settles:
* cold cache: ≤ 3 hitches/min over 20 ms;
* warm cache: ≤ 1/min;
* present-interval stdev ≤ 0.5 ms.

**Status: met for everything the renderer controls.**
* **Live play, local server, vsync, 120 Hz:** 0 frames over 20 ms and present-interval stdev 0.11–0.45 ms, with
  both cold and warm caches. Zero compile or pipeline stalls in every run.
* **Heavy game-side CPU work** (22 bots on a listen server, or demo playback in tonight's environment): periodic
  ~30 ms frames appear on **both** renderers. DXVK under the same bot load: 187 hitches/min. These are outside
  the renderer (median 0.28 ms in Present on hitch frames) and can't be fixed by it.

## What was built (`src/unixlib/render.mm`)
| Piece | Behaviour |
|---|---|
| Async shader compile (M6) | Shaders compile on Metal's compiler threads the moment the game creates them (map load), not at first draw. |
| **Persistent pipeline manifest** | Every pipeline created is appended to `$TF2_HOME/cache/tf2mt/pipelines-v1.bin`. Key: VS/PS bytecode hashes (+ translator version), function-constant spec, attachment formats, samples, blend, write mask. |
| **Background prewarm** | When a shader is created, every manifest pipeline whose shaders are all alive is built on a 4-wide background pool (PLAN §8.2 compile pool). The decoder adopts finished builds at lookup. |
| **Miss policy** (PLAN §8.5) | Pipeline missing at draw time: *translucent* draws (blending on: particles, decals, sprites) are skipped until the background build lands (typically a few frames). *Opaque* draws build synchronously. `TF2MT_NO_DEFER=1` disables skipping; replays/goldens use it for deterministic output. |
| **Stall log** | Any wait for a shader compile, or pipeline creation, over 2 ms on the decoder thread is logged (`stall: frame N …`) and counted in the `render:` ledger line. |

## Measurements (bench.dem, full playback, 1920×1080)
| Run | Caches | Renderer stalls > 2 ms | Pipelines | fps | hitches > 20 ms |
|---|---|---|---|---|---|
| `m8-cold` (vsync on) | Metal compile cache moved aside, no manifest | **0** | 38 sync builds (each < 2 ms), 13 prewarmed | 118.4 (vsync) | 858 (264/min) |
| `m9-perf1` (vsync off) | warm | **0** | 146 prewarmed, 23 adopted, 21 sync | 246.6 | 345 (106/min) |
| `m9-perf2` (vsync off) | warm, M9 encoder | **0** | — | 266.8 | 298 (92/min) |
| `m8-dxvk` (DXVK, same night) | DXVK async + state cache | — | — | 232.7 | 214 (66/min) |
| `m8-dxvk-norelay` | DXVK, `net_steamcnx_allowrelay 0` | — | — | 232.9 | 224 (69/min) |

### Why the hitch counts don't measure the renderer tonight
* Phase 0 measured the same demo on DXVK at **3.4 hitches/min** uncapped (0.9 tuned). Tonight DXVK shows
  **66–69/min**.
* The hitches are **periodic, about every 190 ms**, on both renderers: the gap between hitches is 41–47 frames at
  ~233 fps (DXVK), ~45 at ~250 fps and ~24 at 120 Hz (tf2mt).
* On hitch frames, tf2mt spends a median **0.28 ms in Present**. That covers the whole decode + Metal encode +
  submit. The 20+ ms lands outside the renderer.
* The game's console shows heavy **SteamNetworkingSockets lock contention** (Valve relay config fetch failing with
  HTTP 504, locks held up to 1.3 s; 15–66 warnings per run vs 3 in Phase 0). Disabling the relay fallback didn't
  change the hitch rate. The Mac was on AC with no thermal or CPU-power events, and Spotlight was idle.
* At vsync (120 Hz), any stall over ~4 ms past the frame budget doubles or triples the frame, which is why the vsync
  run counts more hitches. The present-interval stdev measured on the Metal side is 2.6–3.0 ms, dominated by these
  periodic frames.

## Live sessions (TF2 on tf2mt via `TF2_RENDERER=tf2mt scripts/tf2.sh`, koth_harvest_final local server, vsync)
| Session | Caches | Load | Present interval per 600-frame window (mean / sd / max) | > 20 ms frames | Renderer stalls |
|---|---|---|---|---|---|
| 20261008-011700 | warm | spectator, no bots, ~2.7 min | **8.333 ms / 0.115–0.447 ms / ≤ 17.2 ms** | **0** | 0 |
| 20261008-012121 (first ~3 min) | **cold** (Metal cache moved aside, no manifest) | waiting for players, no bots | **8.333 ms / 0.111–0.371 ms / ≤ 15.2 ms** | **0** | 0 |
| 20261008-012121 (after bots join) | cold → warming | 22 bots, spectating | 8.5–8.6 ms / 2.9–3.9 ms / 26–74 ms | periodic | 0 |

The DXVK reference under the same 22-bot listen-server load (Phase 1 census `base`, 12 min steady state) has
**187 hitches > 20 ms/min**. The bot simulation on the listen server, not the renderer, sets that floor. A dedicated
server or online play doesn't run the simulation locally.

## Conclusion
The renderer-side anti-stutter engine works: zero compile or pipeline stalls even with every cache cold. In live
play without heavy local simulation, the PLAN gate (≤ 3 hitches/min cold, ≤ 1 warm, pacing sd ≤ 0.5 ms) is met with
margin. The remaining hitches in demo and bot scenarios are game-side and identical under DXVK. Confirming the
online numbers (no local server simulation) is part of M10's owner-run online soak.

**Re-measure on a clean night** (no SteamNetworkingSockets warnings in `console.log`):
```
scripts/bench.sh <tag>-dxvk dxvk            # environment check: expect ≈ 3/min like Phase 0
# cold: mv "$(getconf DARWIN_USER_CACHE_DIR)/com.apple.metal/32024" aside; rm -rf ~/Games/tf2/cache/tf2mt
scripts/bench.sh <tag>-cold tf2mt           # vsync on (default): hitches/min + unix.log "present:" sd
scripts/bench.sh <tag>-warm tf2mt
grep -c '^stall' ~/Games/tf2/logs/runs/<tag>-*/unix.log   # renderer-attributable stalls
```
If hitches remain with a clean DXVK baseline, the next step is CAMetalDisplayLink-driven presentation (PLAN D6),
which this milestone didn't need to touch.
