# M3 — own adapter + null device (2026-10-07)

Status: **substantially complete; `tools/replay` deferred** (see end).

| M3 criterion | Result |
|---|---|
| tf2mt answers IDirect3D9 itself, no DXVK | **Done**: `src/frontend/adapter.c`; `d3d9_oracle.dll` removed from the installer |
| Same answers as DXVK gave TF2 | **Pass**: `tools/census/adapter-parity.sh`: identity, all 304 caps bytes, 214/214 census queries, 31/31 display modes |
| TF2 reaches a map and plays | **Pass**: full 206 s benchmark demo (66,000 presented frames) and a live map load, tf2mt as the only D3D9 provider |
| Frame time within ±5 % of the null ceiling | **Pass against the same-day control** (below); not comparable with Phase 0's old number |

## Frame-time check (demo `bench.dem`, `fps_max 0`, display sync off, 600-frame block means)
| Run | fps | ms/frame |
|---|---|---|
| Phase 0 null + proxy, measured 2026-10-06 | 570 | 1.75 |
| **Same null + proxy, re-measured today (control)** | **426** | **2.35** |
| **tf2mt device, no presenting** (`TF2MT_NOPRESENT=1`) | **475** | **2.11** (11 % faster than the control: no proxy in front) |
| tf2mt device **with** clear+present of a Metal drawable every frame | ~318 | ~3.1 |

* The machine/game state is ~25 % slower today than on 2026-10-06 for the *identical* control, so Phase 0's absolute number
  cannot be the yardstick; always re-run the control on the day.
* **Presenting costs ~1 ms per frame** (`nextDrawable` + encode + commit, from Rosetta-translated code on the game thread).
  That is the M8/M9 target (present thread, `CAMetalDisplayLink`), recorded here as the first perf-ledger item.

## Findings
* DXVK reports a **different identity and caps depending on the calling program**: inside TF2 the Apple identity
  (vendor 0x106b); from a plain exe an "NVIDIA GeForce 8800 GTX" (0x10de) with different caps. tf2mt reproduces the
  in-TF2 behaviour (ADR-001). Source's choices depend on it; do not "fix" it without a census.
* Caps advertise `vs/ps_3_0` as DXVK does, although TF2 uses the SM2 paths here. Whether Source's path would change if tf2mt
  advertised less is untested; keep identical until M6 can compare images.
* The census tables answer only combinations TF2 asked about; the log prints `UNSEEN ...` for anything else (this also
  prints the census's recorded NO rows). Higher presets/dxlevels need another census pass before M7.
* MSAA answers are policy (Metal sample counts from the device: 2/4/8), not census.

## Not done
* `tools/replay` (capture + replay of D3D9 call streams for pixel diffs). Not needed until M5/M6 (resources/rendering),
  and it is a good fit to build with the real device. Recommend moving it to the start of M5 (decision for the owner).
* Reading Source's `mat_dxlevel`/`mat_hdr_level` through RCON in the tf2mt run failed to print (RCON output has a
  binary byte; fixed with `grep -a`, but the final smoke run used demo mode). Re-run `tools/census/tf2mt-smoke.sh m3b`
  (no DEMO) to confirm both cvars match the DXVK run (`mat_hdr_level 0` in the owner's config).
