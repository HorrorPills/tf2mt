# M6 report — core rendering

Date: 2026-10-07. Gate (PLAN §10): *first correct frames: main menu + koth_harvest_final world geometry,
SSIM ≥ 0.98 vs the DXVK oracle.* **Result: passed**, at SSIM ≥ 0.9992 on every compared frame. That also clears
M7's 0.995 threshold on these scenes. Design: [ADR-005](decisions/ADR-005-rendering.md).

## What was built
| Piece | Files |
|---|---|
| Command stream (frontend → backend) | `src/common/commands.h`; frontend packets + flush points in `src/frontend/device.c` |
| Renderer: state, passes, PSO/DSS/sampler caches, draws, clears, StretchRect, ColorFill, Present | `src/unixlib/render.mm` |
| Shared backend internals, unified submission (uploads + frame, one serial, 2 frames in flight) | `src/unixlib/backend.h`, `src/unixlib/resources.mm` |
| Translator linked into `tf2mt.so`; async shader compile at creation | Makefile, `render.mm` |
| Replay frame dumps (GetRenderTargetData path, same on every provider) | `tools/replay/replay.c` `--dump`, `run-replay.sh DUMP=…` |
| SSIM / diff tool | `tools/replay/ssim.py` |
| Demo capture scenario (koth_harvest_final combat, no MOTD) | `tools/replay/capture.sh <tag> demo` |

## Pixel comparison (replay, same capture on both providers, 1920×1080)
| Capture / frame | Content | SSIM | MAE | pixels > 16 |
|---|---|---|---|---|
| m5-harvest 150 | main menu | **1.0000** | 0.00 | 0.00 % |
| m5-harvest 600–2400 | MOTD over koth_harvest_final | **0.9998** | 0.02 | 0.00 % |
| m6-demo 120–1050 (5 frames) | koth_harvest_final world, combat | **0.9992–0.9994** | 0.03 | 0.02 % |
| m5-loadout 150–3450 (6 frames) | menu, ctf_2fort, loadout screen | **0.9998–1.0000** | ≤ 0.02 | 0.00 % |

The remaining world-frame differences are about 370 scattered single pixels per frame (alpha-tested foliage edges,
float precision); there are no structural differences.

**Bug found by the oracle:** the first comparison gave SSIM 0.94 with a uniform 1-px up-left shift. The half-pixel
correction in PLAN §8.4 has the wrong sign for Metal. D3D9 pixel centres sit at integer coordinates and Metal's at
+0.5, so geometry must move +0.5 px right and down. The texel-aligned UI snapped the half-pixel error to a whole
pixel. After fixing the sign, SSIM is 0.9998–1.0000 (ruling in `docs/semantics.md`).

## Live TF2 on tf2mt (no DXVK anywhere)
| Run | Result |
|---|---|
| `tf2mt-smoke.sh m6live`: local koth_harvest_final server, ~1 min | renders; 250–290 fps; 0 skipped draws; 33 PSOs |
| `DEMO=1 tf2mt-smoke.sh m6demo`: full bench.dem, `fps_max 0`, vsync off | **67,800 frames, ~330 fps mean** (270–344 per 600-frame window, max frame ≈ 33 ms); 17.5 M draws, 0 skipped; 45 PSOs |

These are first numbers, not M9 measurements. Occlusion queries still answer "visible" immediately and EVENT
completes at once (see open items), so the CPU is less constrained than it will be.

## Fixes and rulings made during M6
* Half-pixel sign (above).
* Draws with a VS but no pixel shader (about 10 % of draws: depth/occlusion passes) use a built-in fixed-function
  fragment stage that outputs the interpolated diffuse colour. SSIM was unchanged, which confirms they are
  invisible passes.
* D3D9 `SetRenderTarget(0)` resets viewport and scissor to the target (frontend).
* D3D9 Clear covers the viewport ∩ scissor ∩ rects. A full-target clear becomes the pass's load action; other clears
  draw a quad.

## Open items (for M7 coverage and M8/M9)
* **Queries:** occlusion results are constant "visible" and EVENT is immediate. Real visibility results and the
  frame-latency event are needed for correct flare/sprite culling and pacing (M7/M8).
* **MIPMAPLODBIAS** is ignored (Metal samplers have no LOD bias). The census sees bias 0–3; this needs a shader-side
  bias via function constants (M7).
* **Uploads** execute before the frame's draws, so a texture updated mid-frame after being used in that frame would
  be wrong. Census traffic makes this rare; M8 may move uploads in-stream.
* **Static buffer locks** don't wait for the GPU (D3D9 would). This only matters if a static buffer is rewritten
  while in flight.
* Not exercised by these scenes: MSAA targets (resolve path written, untested), depth StretchRect, ColorFill with a
  rect, user clip plane (water), sRGB write toggling. All are M7 golden scenarios.
* PSO creation and shader specialisation are synchronous on first use (map-load hitches). The M8 anti-stutter work
  is next in line.
* `mat_dxlevel`/`mat_hdr_level` printout (M3 leftover): RCON prints to the client, not the log. Still open; the
  identical replays show Source takes the same path.
