# M5 report — resources, formats, locks (+ tools/replay)

Date: 2026-10-07. Gate (PLAN §10): *replay of 3 scenarios creates every resource without error; upload bandwidth
within ledger targets.* **Result: passed.** Design: [ADR-004](decisions/ADR-004-resources-and-replay.md).

## What was built
| Piece | Files |
|---|---|
| `.t9` capture format | `tools/trace/t9.h` |
| Capture mode in the trace proxy (`TF2MT_TRACE_MODE=capture`) | `tools/trace/capture.c` |
| Scenario capture runner (`-insecure`, local bots, connection guard, config restore) | `tools/replay/capture.sh <tag> harvest\|loadout\|mapchange` |
| Replayer (Windows PE, any provider) | `tools/replay/replay.c` → `build/tools/replay.exe` (`make replay`) |
| Replay runner (DXVK or tf2mt; `VERIFY=1` for byte-exact uploads) | `tools/replay/run-replay.sh <capture> dxvk\|tf2mt` |
| Capture inspector | `tools/replay/t9dump.py <capture> [--id N] [--stats]` |
| Backend resources: buffers, textures, staging ring, rename, upload, readback, verify, stats | `src/unixlib/resources.mm`, new calls in `src/common/unix_calls.h` |
| Frontend: backend-backed buffers/textures/surfaces, DISCARD renaming, Unlock/UpdateTexture/UpdateSurface uploads, GetRenderTargetData, upload ledger | `src/frontend/device.c` |

M3's deferred `tools/replay` is now done.

## Captures (owner's `low` preset, 1920×1080, 22 bots, `fps_max 60`)
| Scenario | Frames | Records | Size | Objects |
|---|---|---|---|---|
| `harvest`: menu → koth_harvest_final, spectating | 2,697 | 6.0 M | 4.3 GB | 73,683 |
| `loadout`: menu → ctf_2fort, spectating, loadout screen | 3,478 | 10.7 M | 4.8 GB | 76,931 |
| `mapchange`: menu → pl_badwater → cp_dustbowl | 5,056 | 11.9 M | 7.8 GB | 123,798 |

Captures are stored under `~/Games/tf2/cache/traces/` (17 GB; outside the repo). The audit hook found no device
method TF2 calls without a recorder.

## Replay results
| Scenario | DXVK (oracle) | tf2mt | tf2mt upload verification |
|---|---|---|---|
| harvest | 0 failed calls, 0 missing objects | **0 failed**, 0 missing | **29,403 / 29,403** byte-exact |
| loadout | 0 failed, 0 missing | **0 failed**, 0 missing | **31,289 / 31,289** byte-exact |
| mapchange | 0 failed, 0 missing | **0 failed**, 0 missing | **52,904 / 52,904** byte-exact |

Every resource TF2 creates gets real Metal storage on tf2mt: up to 41,684 textures, 6,044 VBs, 5,424 IBs, cubes,
volumes, render targets and depth surfaces.

## Upload ledger (tf2mt, replay without verification)
| Measure | harvest | loadout | mapchange |
|---|---|---|---|
| Map-load texture uploads | 3,542 MB / 28,459 calls in **238 ms** | 3,572 MB in 223 ms | 3,588 MB in 235 ms |
| Steady-state uploads | 5.7 MB / 600 frames in 6.1 ms (**0.01 ms/frame**) | 6.6 MB / 600 frames in 0.4 ms | 6.6 MB / 600 frames in 0.4 ms |
| DISCARD renames / new backings | 4,801 / 14 | 2,404 / 15 | 2,401 / 11 |
| Live after load | 5.0k buffers 80 MB, 7.2k textures 2.2 GB | 5.7k / 88 MB, 7.6k / 2.2 GB | 5.5k / 90 MB, 7.5k / 2.3 GB |

**Targets:** PLAN sets no number, so these are the ledger targets for M5:
* map-load upload throughput ≥ 2 GB/s: **measured ~15 GB/s**;
* steady-state upload work ≤ 0.5 ms/frame: **measured ≤ 0.01 ms/frame**;
* dynamic renaming must not allocate per frame: **11–15 backings for ~2,400–4,800 discards**;
* texture memory must return after a map change: **2.2 GB before and after**.

Whole-replay speed (tf2mt 530–770 fps vs DXVK 149–171 fps) isn't comparable yet, because tf2mt doesn't draw until
M6.

## Notes, open items
* Replay lifetimes follow the captured refcounts (ADR-004 §2). DXVK's public refcounts differ from what a pure
  mirror would produce (the "release-count differences" line in reports; informational only).
* `UpdateTexture` uploads all destination levels (no dirty-rect tracking). That is 3.5 GB per map load at this
  preset, cheap at 15 GB/s. Revisit only if streaming shows up in M8 hitch data.
* Sampling swizzles (X8 → alpha 1, L8, A8L8, V8U8) and sRGB views are applied when textures are bound (M6).
* `GetRenderTargetData` reads back synchronously but contents are only meaningful once M6 renders.
* Not exercised by these captures: `Reset` (resolution change), MSAA render targets, the dynamic A16B16G16R16 HDR
  texture (`mat_hdr_level 2`). They need captures at other settings before M7.
