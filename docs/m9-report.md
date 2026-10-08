# M9 report — performance pass (G2)

Date: 2026-10-08. Gate (PLAN §10): *§15 targets met on the demo; layer CPU ≤ 2 ms/frame, GPU ≤ 4 ms; else a
documented ceiling and stop.*

## Results (bench.dem, full playback, 1920×1080, owner's `low` preset, vsync off)
| Measure | DXVK (same night) | tf2mt M6 | tf2mt M9 | Target |
|---|---|---|---|---|
| Average fps | 232.7 | ~247 | **266.8** | ≥ 120 ✓ |
| p50 frame | 3.81 ms | 3.61 ms | **3.31 ms** | — |
| Backend CPU (decode + Metal encode + submit) | — | 1.10 ms/frame | **0.80 ms/frame** | layer ≤ 2 ms ✓ |
| GPU time per frame (`GPUEndTime − GPUStartTime`) | — | 1.00 ms | **0.94 ms** | ≤ 4 ms ✓ |
| Time in Present (p50 / p99) | — | 0.235 / 1.27 ms | 0.195 / 1.63 ms | — |
| 1 % low / p99.9 | 47.6 fps / 25.8 ms | 45 fps / 24.9 ms | 47.3 fps / 24.3 ms | ≥ 100 fps / ≤ 12 ms ✗ |

At vsync (the default), tf2mt holds the 120 Hz panel: 118.4 fps average, p50 8.3 ms.

The **1 % low and p99.9 targets fail on both renderers alike**. They are set by tonight's environmental periodic
hitches (docs/m8-report.md), not by the layer. Layer CPU and GPU time are each under a quarter of their budgets.

## Work done
* **Encoder redundancy cache:** pipeline, depth-stencil, raster, viewport, scissor, vertex buffers, textures,
  samplers and driver constants are only re-sent when changed. Shader constants are re-sent only after a
  `Set*ShaderConstant` changed them. Result: backend CPU −27 %, frame rate +8 %. `TF2MT_NO_ENCCACHE=1` disables it
  for diagnosis.
* **Ledger:** `ledger-perf:` line every 600 frames (backend encode ms/frame, GPU ms/frame and max, submissions),
  alongside the upload ledger (M5) and the `render:` line (draws, caches, prewarm, stalls).
* **Bug found while validating the cache: MSL ABI layout.** The prelude declared `float3`/`uint3` padding in
  `tf2mt_ps_driver` and `tf2mt_int_consts`. In MSL those types are 16-byte aligned, so the shader-side structs were
  96 and 288 bytes while the CPU sent 80 and 272. Shaders read `lod_bias` from the wrong offsets, partly past the
  bound data, so the output depended on unrelated bytes. Fixed, with `static_assert`s in the prelude for all three
  ABI structs.
  * m7-high golden: 0.9971 → **0.9993**;
  * m7-hdr2: 0.9975 → **0.9992–0.9996**;
  * **full golden set minimum: 0.9991** (was 0.9971).

## Not done, and why
* Separate encoder thread, argument buffers, constant ring, AIR emitter (PLAN M9 menu): backend CPU is already
  0.8 ms/frame against a 2 ms budget, and D2's revisit trigger (> 1.2 ms backend CPU at 2,500 draws) isn't hit at
  ~250 draws/frame. Revisit only if a higher preset or busier scenes push the ledger toward the budget.
* PLAN's G2 "else documented ceiling and stop" doesn't apply: the targets the layer controls are met. The
  1 %-low/p99.9 targets must be re-measured together with M8's gate on a clean night.

## Addendum 2026-10-08: encoder thread + pipeline policy (after the owner's first online sessions)
Online Medium play showed 87 hitches/min (uncapped), mostly synchronous first-use pipeline builds (241 stalls,
median 19 ms) and a game thread that spent ~1.5 ms/frame inside tf2mt. Changes:
* **Encoder thread** (PLAN §8.2): SUBMIT copies the batch and returns. One unix-side thread decodes and encodes,
  waits for drawables and frames in flight without holding the lock, and the game is ≤ 1 presented frame ahead.
  Destruction and DISCARD buffer switches are stream commands; queries carry generations and are polled lock-free.
  `GetData(FLUSH)` requests a submit instead of waiting.
* **Pipeline policy:** DXVK-async style. A draw whose pipeline isn't built yet is skipped until a background build
  lands; it is never built on the frame. A fixed 4-thread build pool with an on-demand queue ahead of prewarm
  replaces GCD blocks, which had starved on-demand builds and deferred ~40 % of draws for a whole run. The shader
  owner is looked up through the object, since TF2 creates identical shaders more than once.

| Medium, bench.dem | DXVK (uncapped) | tf2mt (uncapped) | tf2mt (vsync 120) |
|---|---|---|---|
| Average fps | 204.6 | 159.1 | 119.9 |
| p99 / p99.9 frame | 13.1 / 15.0 ms | **7.9 / 8.8 ms** | 9.6 / 11.3 ms |
| 1 % low | 59.7 fps | **117.8 fps** | 96.1 fps |
| Hitches > 20 ms | 3.3/min | **0.3/min** | **0.6/min** |
| Present interval sd (Metal side) | — | — | **0.07–0.36 ms** |
| Game-thread time inside tf2mt | — | **0.01 ms/frame** (was 1.25) | — |
| Renderer stalls / deferred draws | — | 0 / 218 total | 0 |

With these runs M8's gate is met on the demo: warm ≤ 1 hitch/min and pacing sd ≤ 0.5 ms. tf2mt's average fps is
22 % below DXVK's uncapped (median frame 6.2 vs 4.2 ms), but time spent inside the layer's calls is now ~0. The
remaining gap is the frontend's per-call cost or thread interaction: an open item for further profiling. Goldens
and translator tests are unchanged.
