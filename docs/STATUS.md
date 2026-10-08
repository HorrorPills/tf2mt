# tf2mt — status and handoff (read this first)

Last updated 2026-10-08 (v0.3.0 prepared: low-latency pacing + launcher auto-updater). Owner rules: do **not** commit or push to GitHub unless the owner asks. The agent never
joins online servers itself; online tests are played by the owner (see "Safety").

## Where things stand
| Track | State |
|---|---|
| **Next release (v0.3.0, prepared)** | Low-latency pacing default (game ahead 0, 2 drawables; frame start → on screen ~41 → ~25–31 ms, `latency:` lines in unix.log, docs/mouse-input.md) and an in-app auto-updater (GitHub latest release, SHA-256 digest check, in-place swap; README "Updates"). |
| **Released app (v0.2.0)** | Public at github.com/HorrorPills/tf2mt. Native Metal renderer on by default, mastercomfig Low default (installed on first Play), mouse fix + Friends-offline on, launcher tabs Settings/Setup/Debug. Setup needs nothing beyond stock macOS (mouse patch tool is bash since v0.2.0). v0.1.0: DXVK-only. |
| **Renderer M0** (Phase 0) | Done: `docs/phase0-report.md`, gate G0 passed |
| **Renderer M1** (census) | Done: `docs/census-summary.md` (scope), `docs/census-report.md` (tables), ADR-001 (SM2 path first). Corpus: `corpus/*.bin` 1,190 live shaders + `corpus/vcs/` 318,788 extracted (git-ignored; regenerate with `tools/census/vcs.py`) |
| **Renderer M2** (Wine spike) | Done, **G1 passed**: `docs/m2-report.md`, ADR-002. 119.65 fps over 60 s, input verified by owner |
| **Renderer M3** | Done except `tools/replay` (deferred): `docs/m3-report.md` |
| **Renderer M4** (shader translator) | Done: `docs/m4-report.md`, ADR-003, `docs/semantics.md`. `make test-translate` |
| **Renderer M5** (resources + capture/replay) | Done: `docs/m5-report.md`, ADR-004 |
| **Renderer M6** (core rendering) | Done: `docs/m6-report.md`, ADR-005. SSIM ≥ 0.9992 vs DXVK; TF2 runs live on tf2mt (~330 fps bench demo) |
| **Renderer M7** (coverage) | Done: `docs/m7-report.md`. 26 goldens ≥ 0.9971; 50× map-change soak; Reset; leak fixed |
| **Renderer M8** (anti-stutter) | Done for the renderer: `docs/m8-report.md`. 0 compile stalls cold/warm; live pacing sd 0.11–0.45 ms, 0 frames > 20 ms without heavy local simulation; bot/demo hitches are game-side (DXVK same) |
| **Renderer M9** (performance) | Done: `docs/m9-report.md`. 267 fps vs DXVK 233; backend 0.8 ms, GPU 0.94 ms/frame; MSL ABI layout bug fixed (goldens min 0.9991) |
| **Renderer M10** (packaging) | Done except the owner-run online soak: `docs/m10-report.md`. Launcher option (default on since v0.2.0) + `TF2_RENDERER=tf2mt`, per-session install/uninstall, VAC-secure (owner plays online) |

## How the renderer is wired (ADR-002)
`TF2/d3d9.dll` (forwarder, `src/frontend/d3d9_forward.*`) → builtin `tf2mt.dll` (`src/frontend/device.c`, PE, stamped by
`tools/wine/mark_builtin.py`) → unix calls (`src/common/unix_calls.h`) → `tf2mt.so` (`src/unixlib/tf2mt_unix.m`) →
`macdrv_functions` (exported by this Wine build) → `CAMetalLayer` on TF2's window.
Build: `make frontend unixlib`. Install/remove: `scripts/layer-install.sh` / `scripts/layer-uninstall.sh` (always
uninstall afterwards: the owner plays on DXVK). Logs: `TF2MT_UNIX_LOG=<unix path>`, `TF2MT_TRACE_DIR`+`TF2MT_TAG`.
Since M6 the device renders: frontend command stream (`src/common/commands.h`) → `src/unixlib/render.mm` (Metal).
IDirect3D9 caps/format answers come from tf2mt's own adapter (`src/frontend/adapter.c`), no DXVK oracle.

## Tools (all in-repo)
* `tools/trace` (`build/trace/d3d9.dll`): D3D9 proxy over DXVK. Timing mode (frame CSV, camera-change column, Sleep
  probe) or `TF2MT_TRACE_MODE=census`. `tools/null`: Phase-0 null renderer.
* `scripts/bench.sh <tag> <dxvk|null>`: demo benchmark (`tf/bench.dem`); `tools/bench/analyze.py`, `hitches.py`.
* `tools/census/run-census.sh <tag> [args]` and `run-all.sh`: bots + map cycle via RCON; `report.py` → docs.
* `tools/bench/rcon.py`: RCON to the local listen server (`-usercon +rcon_password tf2mt`, `+sv_lan 1`).
* `tools/bench/repro-loadout.sh`, `mouse-test.sh`, `mouse-report.py`.

## Safety & traps (learned the hard way)
* **Online:** `-insecure` does **not** work (tf.exe strips it before tf_win64.exe), so test launches run VAC-secure;
  they only use local listen servers or demos, and `run-census.sh` kills TF2 on any non-local connection. The owner once matchmade into Casual during a census run (no VAC messages seen).
* **Display sleep / screen lock** invalidates measurements: `scripts/bench.sh` refuses to run, keeps
  `caffeinate -d`; `tools/bench/displaystate` checks.
* **Steam must be logged in** before TF2 starts, or it falls back to -insecure mode and crashes during load. Wait for
  a *new* "Logged On" line in `Steam/logs/connection_log.txt`.
* `pgrep -f 'Team Fortress 2.tf_win64'` also matches your own shell command: use `tf2_running`/`tf2_pids`
  (`scripts/_env.sh`).
* `scripts/verify-env.sh` ends with `wineserver -k` (closes Steam).
* macOS `wc -l` pads with spaces (`tr -d ' '` before `tail -n +N`); BSD sed has no `\b`.
* The `sample` profiler under Rosetta perturbs TF2 badly (it caused fake stalls). Don't trust profiles taken with it.
* SwiftUI built with `swiftc` (no Xcode): `@State` macro unavailable; use `@AppStorage` / `@StateObject`.
* Census mode patches DXVK resource vtables in-process (DXVK = black-box oracle; the game is never touched).
* TF2 saves cvars to `config.cfg` on exit: variant runs back it up and restore it (`run-census.sh`).

## M3 — done except tools/replay (see docs/m3-report.md)
* Own `IDirect3D9Ex` (`src/frontend/adapter.c`), census-derived tables (`tools/census/gen_caps.py` → `caps_table.h`,
  `tests/golden/adapter_queries.tsv`); parity vs the recorded in-game answers: `tools/census/adapter-parity.sh`
  (identity, caps, 214 queries, 31 modes all identical). DXVK-from-a-plain-exe is NOT a valid reference (different identity).
* Live runs: `tools/census/tf2mt-smoke.sh <tag>` (map; `DEMO=1` plays the benchmark demo; `TF2MT_NOPRESENT=1` skips Metal).
  tf2mt device = 475 fps vs 426 fps same-day control; with presenting ~318 fps (present costs ~1 ms: M8/M9 item).
* Leftovers: (1) `tools/replay`: done in M5; (2) rerun `tf2mt-smoke.sh m3b` to print
  `mat_dxlevel`/`mat_hdr_level` and confirm they match DXVK; (3) nothing committed (owner rule).

## M4 — done (see docs/m4-report.md)
* `src/translate/`: decoder (`sm.h`, `sm_decode.cpp`), MSL emitter (`msl.h`, `msl_emit.cpp`, `msl_prelude.inc`), shader
  ABI for the backend (`msl_abi.h`: bindings, varying slots, vertex-pulling fetch table, function constants), CPU
  reference interpreter (`interp.*`). Design: ADR-003. Every semantic ruling: `docs/semantics.md`.
* Tests: `make test-translate` = golden opcode cases (`tests/unit/opcodes.mm`, GPU and CPU vs hand values) +
  GPU-vs-CPU differential test of the live corpus (`tools/translate/difftest.mm`) + Metal compile, specialisation and
  VS/PS link of the live corpus (`tools/translate/mslbatch.mm --link N`). Full corpus: `build/tools/mslbatch --scope
  all corpus corpus/vcs` (~1-2 h). `build/tools/smstat` = feature census of any shader set.
* Backend (M5/M6) must: bind per `msl::Reflection`; fill `tf2mt_vs_driver` (half-pixel, clip plane 0, fetch table
  from the vertex declaration, stream sizes); compile with `MTLMathModeRelaxed`; set fc 0/1 per PSO.
* Open semantic items marked *oracle* in `docs/semantics.md` (legacy multiply on/off, unwritten VS outputs, alpha-test
  precision, texldp w<=0) get confirmed against DXVK pixels at M6/M7.

## M5 — done (see docs/m5-report.md)
* Capture: `tools/replay/capture.sh <tag> harvest|loadout|mapchange` (TF2 runs ~2-3 min, offline/-insecure) →
  `~/Games/tf2/cache/traces/<tag>/capture-<tag>.t9`. Replay: `tools/replay/run-replay.sh <t9> dxvk|tf2mt`
  (`VERIFY=1` = byte-exact upload check). Inspect: `tools/replay/t9dump.py`.
* All three captures replay on tf2mt with 0 failed calls; 113,596 texture uploads verified byte-exact.
* Backend resources: `src/unixlib/resources.mm` (shared buffers + DISCARD renaming, private textures + staging blits).
  Upload ledger line in the tf2mt log every 600 frames.
* For M6: the replayer is the test driver (no game needed); add a pixel-dump mode to replay (read back RT0 at chosen
  frames on both providers) for the SSIM gate. Sampling swizzles/sRGB views per `Fmt` in resources.mm.

## M6 — done (see docs/m6-report.md)
* Pixel gate: `DUMP=f1,f2 tools/replay/run-replay.sh <t9> dxvk|tf2mt` then `.venv/bin/python tools/replay/ssim.py
  <dir>/frames-dxvk <dir>/frames-tf2mt --diff <out>`. Gate capture: `~/Games/tf2/cache/traces/m6-demo` (bench demo).
* Live: `tools/census/tf2mt-smoke.sh <tag>` (map) / `DEMO=1 ...` (bench demo) now render through Metal.
* M7 starts from the open-items list in docs/m6-report.md.

## M8–M10 — done overnight 2026-10-08 (owner asleep, full autopilot)
* Reports: docs/m7-report.md … docs/m10-report.md. All golden frames ≥ 0.9991 (`tools/replay/goldens.sh`).
* Play on tf2mt: launcher option "Native Metal Renderer" (default on) or `TF2_RENDERER=tf2mt scripts/tf2.sh`.
  Owner decision 2026-10-08: online VAC play with tf2mt.
* Live online jitter (Medium, vsync 120 Hz): game-side frame-time variance (frames whose own work exceeds 8.3 ms),
  present on DXVK as well (~350 missed refreshes/min vs 216 on tf2mt). Low preset is the recommended default.
* Environment note: on 2026-10-08 the bench demo showed ~70 periodic hitches/min on DXVK too (SteamNetworkingSockets
  contention in console logs); compare against a fresh DXVK baseline before trusting any hitch number.
