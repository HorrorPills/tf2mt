# M7 report — full §9 coverage

Date: 2026-10-08. Gate (PLAN §10): *all goldens SSIM ≥ 0.995 (documented FP exceptions); 50× map-change soak
without leak/crash; Reset tests.* **Result: passed.**

## Golden set (`tests/golden/frames.tsv`, runner `tools/replay/goldens.sh`)
26 frames from 8 captures, tf2mt vs DXVK on the identical call stream (replay, 1920×1080):

| Capture | Covers | Frames | SSIM |
|---|---|---|---|
| m5-harvest | main menu, MOTD over koth_harvest_final | 3 | 0.9998–1.0000 |
| m5-loadout | ctf_2fort (water, **user clip plane**), loadout screen | 4 | 0.9998–1.0000 |
| m5-mapchange | pl_badwater → cp_dustbowl | 3 | 0.9998 |
| m6-demo | koth_harvest_final combat (low preset) | 5 | 0.9992–0.9994 |
| m7-hdr2 | `mat_hdr_level 2` (HDR, dynamic A16B16G16R16, tonemap readbacks) | 3 | 0.9975–0.9988 |
| m7-msaa4 | `mat_antialias 4` (MSAA back buffer, resolve via StretchRect) | 2 | 0.9988 |
| m7-high | render-to-texture shadows, depth shadows, phong, specular, bump, expensive water, picmip 0, HDR | 3 | 0.9971 |
| m7-reset | `mat_setvideomode` 1920×1080 → 1280×720 → back (device **Reset**) | 3 | 0.9988–0.9998 |

The minimum is 0.9971 (m7-high). Its difference image shows low-amplitude texture-filtering variation on finely
detailed surfaces (MAE 0.58/255, 0.03 % of pixels > 16). There are no structural differences, and every frame
clears the 0.995 gate, so the FP-exception clause isn't needed.

## Soak (`tools/replay/soak.sh`, TF2 live on tf2mt, local server)
* **50 map changes** cycling koth_harvest_final, ctf_2fort, pl_badwater, cp_dustbowl, cp_process_final and
  pl_upward: survived, no crash.
* **Leak found and fixed.** The first soak showed process memory growing by ~128 MB per map change (1.8 → 8.1 GB),
  while the backend's live object counts stayed flat. Unix calls ran without an autorelease pool. Wine threads have
  no run loop, so every autoreleased Metal object (command buffers, encoders, descriptors) lived forever. Every
  unix call now runs in `@autoreleasepool` (`src/unixlib/tf2mt_unix.m`).
* After the fix (15 cycles): RSS 1.74 → 2.09 GB, about 25 MB per cycle, matching **DXVK's own baseline** (1.76 →
  2.13 GB, `PROVIDER=dxvk tools/replay/soak.sh 15`). The remaining growth is TF2's.
* Backend live set after each load stays flat (about 3.7–4.0k textures, 1.2–1.7 GB; 5.0–5.5k buffers, ~88 MB).

## Features added or fixed in M7
* **Queries:**
  * OCCLUSION counts samples on the GPU (visibility result buffer, 16k result slots).
  * EVENT completes with its command buffer.
  * `GetData(FLUSH)` submits the partial frame when needed.
  The frame-latency limiter and occlusion culling now see real results (live soak at ~250 fps).
* **MIPMAPLODBIAS:** applied in the shader (`bias()` from a PS driver constant), because Metal samplers have no
  LOD bias.
* **MSAA:** resolve targets are cached per (format, size), so there's no per-frame allocation. The replay's frame
  dumps resolve before GetRenderTargetData.
* **Pipeline cache keys:** pipelines are dropped when their shader is destroyed, so a reused address can't hit a
  stale pipeline.
* **Capture deadlock fixed (tools):** resource hooks no longer take the capture lock on DXVK worker threads (HDR map
  loads hung).
* **Capture scenarios:** `demo` (with `ARGS=` cvars) and `reset`. Bench and soak runners take a `tf2mt` provider.

## Not covered (owner's settings never use these; census confirms)
* Flashlight shadows are only partly exercised: the bench demo has no flashlight, though depth-shadow paths are in
  m7-high.
* MvM, `mat_queue_mode 2` (multi-threaded rendering stays as configured by the owner's preset), alt-tab.
* Online play: never tested by design (VAC hygiene). That is owner-run in M10.
