# tf2mt — a Team Fortress 2–exclusive Direct3D 9 → Metal renderer backend

Document status: **DEFINITIVE PLAN v2** (2026-10-06). No production code exists yet. Written by Claude Sonnet 5.5 for hand-off to a stronger model.
Owner goal: TF2 running **smoothly at 120 fps** (120 Hz display), no hitching, no input stutter.
Owner rules: (1) **everything is written from scratch, directly for TF2's requirements and standards** — no dx9mt, no DXMT, no DXVK, no MoltenVK in the shipped path; (2) **TF2 only** — nothing a TF2 trace never exercises gets implemented; (3) the owner does not want to be asked questions during execution except where §14 says so.

---------------------------------------------------------------------------
## 1. PATHS (all verified 2026-10-06; `~` = `~`)

Two roots only: the **game environment** (`~/Games/tf2`) and the **source repo** (`~/Code/tf2mt`).

### 1.1 Game environment — `~/Games/tf2/`
| What | Path |
|---|---|
| Environment script — **always `. ~/Games/tf2/env.sh` first** (sets `TF2_HOME WINEPREFIX WINE WINESERVER STEAM_DIR TF2DIR DYLD_FALLBACK_LIBRARY_PATH WINEDEBUG MVK_CONFIG_LOG_LEVEL`) | `~/Games/tf2/env.sh` |
| Wine runtime (Sikarugir **Wine 10.0**, `x86_64-wow64`, runs under Rosetta 2) — 1 GB; **our private copy, we may modify/replace anything in it** | `~/Games/tf2/wine/` |
| `wine`, `wineserver` | `~/Games/tf2/wine/bin/` |
| Wine PE DLLs (x86_64 / i386) | `~/Games/tf2/wine/lib/wine/x86_64-windows/` , `.../i386-windows/` |
| Wine unix-side libraries (Mach-O named `*.so`, x86_64) incl. `winemac.so` (the macOS driver) | `~/Games/tf2/wine/lib/wine/x86_64-unix/` |
| Wine native libs (FreeType, etc.) | `~/Games/tf2/wine/lib/` |
| Bundled third-party stacks (**present only as the baseline/oracle; not used by tf2mt**): DXVK 2.4.1, MoltenVK 1.4.1, DXMT 0.80 | `~/Games/tf2/wine/share/dxvk/`, `~/Games/tf2/wine/lib/libMoltenVK.dylib`, `winemetal.{dll,so}` |
| **Wine prefix** (`WINEPREFIX`, 33 GB: Windows registry + Steam + TF2) | `~/Games/tf2/prefix/` |
| Registry hives | `~/Games/tf2/prefix/{system.reg,user.reg,userdef.reg}` |
| Prefix system DLL dirs (a **native DXVK `d3d9.dll`** sits here today and wins over Wine's builtin) | `~/Games/tf2/prefix/drive_c/windows/system32/d3d9.dll` (64-bit), `.../windows/syswow64/d3d9.dll` (32-bit) |
| **Steam** (`C:\Program Files (x86)\Steam`; also `$STEAM_DIR`) | `~/Games/tf2/prefix/drive_c/Program Files (x86)/Steam/` |
| Steam client exe | `$STEAM_DIR/steam.exe` |
| TF2 app manifest (appid **440**, buildid 25738890, StateFlags 4 = fully installed) | `$STEAM_DIR/steamapps/appmanifest_440.acf` |
| Steam login/config (already logged in; account id <accountid>) | `$STEAM_DIR/config/loginusers.vdf`, `$STEAM_DIR/config/config.vdf`, `$STEAM_DIR/userdata/<accountid>/` |
| TF2 launch options (edit only with Steam closed) | `$STEAM_DIR/userdata/<accountid>/config/localconfig.vdf` → `-novid -nojoy -nosteamcontroller -nohltv -particles 1` |
| **TF2 GAME FILES** (`C:\Program Files (x86)\Steam\steamapps\common\Team Fortress 2`; `$TF2DIR`) — 31 GB | `~/Games/tf2/prefix/drive_c/Program Files (x86)/Steam/steamapps/common/Team Fortress 2/` |
| 64-bit game exe (what really runs) | `$TF2DIR/tf_win64.exe` |
| 32-bit stub (`tf.exe`; re-execs `tf_win64.exe` even with `-32bit`; the Steam-launched entry point) | `$TF2DIR/tf.exe` |
| **The D3D9 consumer we serve** (Valve's shader API) | `$TF2DIR/bin/x64/shaderapidx9.dll` ; siblings `bin/x64/{engine,launcher,materialsystem,stdshader_dx9,studiorender,vguimatsurface}.dll` |
| Valve shader containers (`.vcs`) — **packed inside VPKs, no loose files** | `$TF2DIR/hl2/hl2_misc_*.vpk`, `$TF2DIR/tf/tf2_misc_*.vpk` (extract with our own VPK reader, `tools/census/vpk`) |
| TF2 game dir (content, maps, cfg) — 28 GB | `$TF2DIR/tf/` ; maps `$TF2DIR/tf/maps/` ; benchmark map `koth_harvest_final.bsp` |
| Config | `$TF2DIR/tf/cfg/config.cfg` and Steam-cloud copy `$STEAM_DIR/userdata/<accountid>/440/remote/cfg/config.cfg` |
| mastercomfig (preset `medium`; addons flat-mouse, null-canceling-movement) | `$TF2DIR/tf/custom/` |
| Console log with `-condebug` | `$TF2DIR/tf/console.log` |
| Backups | `~/Games/tf2/backups/` : `parallels-original-config/` (user's original TF2 `config.cfg`, Steam-cloud config, saved launch-option file from the old VM — contains their binds/sensitivity), `prefix-registry-before-reorg/` (registry hives before path repair) |
| Logs (scripts write here) / persistent caches | `~/Games/tf2/logs/` , `~/Games/tf2/cache/` |

### 1.2 Source repo — `~/Code/tf2mt/` (git initialised, nothing committed)
```
PLAN.md                  this file
README.md                (create in M2)
docs/                    census-report.md, semantics.md, perf-ledger.md, phase0-report.md
docs/decisions/          ADR-NNN-*.md (one per decision change)
docs/baseline-parallels/ 35 text reports from the old Parallels/Windows VM era (context only)
src/frontend/            x86_64 PE d3d9.dll: COM objects, caps, state tracker, command encoder (C)
src/common/              ring buffers, arenas, hashing (xxh3 clone, own implementation), logging, config
src/translate/           D3D9 SM2/3 bytecode parser → IR → MSL emitter (C++20)
src/backend/             pass planner, Metal encoder, PSO cache, resource heaps, present (ObjC++/C++20)
src/unixlib/             Wine unix-side glue (x86_64 Mach-O "d3d9.so"): unix-call table, window/layer bridge
tools/trace/             recorder d3d9.dll (chain-loads the DXVK oracle)
tools/replay/            replay exe (Windows x86_64 PE)
tools/census/            trace → coverage matrix; VPK + VCS reader
tools/bench/             demo runner, frame ledger parser, SSIM diff
tools/analysis/          legacy_parallels_analyze.py (percentile/1%-low/spike logic to port)
tests/{unit,golden,soak} translator opcode tests; golden frames; long-running soak
scripts/                 _env.sh steam.sh tf2.sh stop.sh backup-env.sh verify-env.sh (all working)
corpus/                  extracted shader bytecode corpus (git-ignored *.bin)
```
Scripts (all executable, source `~/Games/tf2/env.sh` via `_env.sh`): `scripts/steam.sh` (start Steam), `scripts/tf2.sh [args]` (launch TF2 with the owner's launch options), `scripts/stop.sh` (`wineserver -k`), `scripts/backup-env.sh` (APFS clone of `~/Games/tf2` → `~/Games/tf2.backup-<timestamp>`, instant, ~free), `scripts/verify-env.sh` (existence checks + `wine --version` + `wine cmd /c ver`; last run: all OK).

### 1.3 Removed from this Mac (do not look for these)
Parallels Desktop + the Windows VM, CrossOver + its bottle, PixelPort (app + 34 GB data), the dx9mt repo, the old `~/Downloads/tf2diag` toolbox (its `analyze.py`, config backups and reports were salvaged into the paths above; a few root-owned CSVs remain in `~/Downloads/tf2diag` and need `sudo rm -rf` by the owner).

### 1.4 Toolchain on the Mac
Apple clang (Command Line Tools **only — no Xcode, so no `metal`/`metallib` compiler, no `xctrace`, no Instruments**), `make`, Homebrew `mingw-w64` (`/opt/homebrew/bin/x86_64-w64-mingw32-gcc` — the PE frontend compiler; `i686-` also installed, unused), `python3` (venv with pandas/numpy is *not* present any more — recreate under `~/Code/tf2mt/.venv`), `git`, `swift`. Runtime Metal source compilation (`MTLDevice newLibraryWithSource:`) works without Xcode.

---------------------------------------------------------------------------
## 2. VERIFIED FACTS (inspect/measure, don't re-derive)

| # | Fact | How verified |
|---|---|---|
| F1 | Mac: M1 Max, 10 CPU cores (8P+2E), 24-core GPU, 32 GB; macOS 27.0.1; display "Odyssey G40B" offered at **120 Hz and 60 Hz only** (no 240 Hz) | CoreGraphics mode list |
| F2 | TF2 is **64-bit only now**. `tf.exe` is a 32-bit stub that spawns `tf_win64.exe` (even with `-32bit`). Consequently **all components of tf2mt that load into the game are x86_64 PE**. | Wine `+loaddll` trace |
| F3 | The renderer is `bin/x64/shaderapidx9.dll`; it imports `Direct3DCreate9` (and references `Direct3DCreate9Ex`). FOURCCs present in its strings include `ATI1N ATI2N ATOC DF16 DF24` (list was truncated; finish in census). Source loads the system `d3d9.dll`. | `strings` |
| F4 | Current graphics path: **DXVK 2.4.1 → MoltenVK 1.4.1 → Metal** inside Wine 10 (log `D3D9DeviceEx::ResetSwapChain`). The prefix has a *native* DXVK `d3d9.dll` in `system32`. | game log, file listing |
| F5 | Wine is an **x86_64 process under Rosetta 2**. Our unix-side library must therefore be an **x86_64 Mach-O**; Metal framework internals still run natively. | `RUNTIME.json`: `arch: x86_64-wow64` |
| F6 | `winemac.so` contains Wine's Metal-view helpers: strings `macdrv_view_create_metal_view`, `macdrv_view_get_metal_layer`, `macdrv_view_release_metal_view`, `macdrv_create_metal_device`; ObjC classes `WineWindow`, `WineContentView`; exported data symbols `_macdrv_functions`, `___wine_unix_call_funcs`. ⇒ obtaining a `CAMetalLayer` for a Wine window is supported by Wine itself. | `nm`, `strings` on `x86_64-unix/winemac.so` |
| F7 | The runtime ships `winemetal.dll` + `x86_64-unix/winemetal.so` (DXMT's bridge). It proves the **"builtin PE d3d-style DLL + same-named unix `.so`"** mechanism works on this Wine; we reuse the *mechanism* (Wine's public builtin/unix-call design), not DXMT's code. | file listing |
| F8 | **No Xcode:** `xcrun metal` fails. Shaders must be compiled at runtime from MSL text (works) or emitted as AIR by us (later optimization). | `xcrun -find metal` |
| F9 | TF2's shaders live in VPKs as Valve `.vcs` combo containers (SM 2.0b / 3.0 token streams). No loose `.vcs`. | `find` = 0 |
| F10 | Earlier (Parallels, now removed) measurements: TF2 was **CPU-bound on the emulated x64 main thread (≈86% of a core) with the render thread at ≈66%, GPU ≈15%**; offline bench on `koth_harvest_final` ≈ 118 fps with p99 ≈ 10.7 ms. The same "game code runs under emulation" limit applies to Rosetta. | `docs/baseline-parallels/` |
| F11 | A previous DLL-override test **loaded `C:\windows\system32\d3d9.dll` (DXVK) despite our DLL beside the exe — the test DLL was 32-bit i686 and the process 64-bit, so Wine skipped it.** This does *not* prove the override mechanism is broken; re-test with an x86_64 PE (M2). | trace + analysis |
| F12 | cvars sent to an already running game via `steam -applaunch … +exec` are **not** applied; only a fresh process picks up config. Always restart between benchmark variants; use unique run tags. | repeated observation |

---------------------------------------------------------------------------
## 3. HONEST RISK STATEMENT (read before investing)

A renderer backend removes only **translation overhead** (DXVK/MoltenVK CPU time, pipeline/shader compile hitches, extra copies, presentation pacing). It cannot accelerate TF2's own x64 code under Rosetta. 120 fps needs **≤ 8.33 ms/frame**; F10 says TF2's own work was ≈ 7 ms/frame under the old emulation. The project is therefore gated:

* **Gate G0 (Phase 0)** — measure the *game-only ceiling* (null renderer), the *tuned-DXVK* result and the *share of frame/hitches caused by the translation stack*. Proceed only if all three conditions in §6 hold. If not, the deliverable is a data-backed ceiling report + the tuned-DXVK configuration, and the project stops.
* **Gate G1 (M2)** — proof that our DLL can load into TF2 and present on its window at 120 Hz. If Wine integration cannot be done without patching Wine, stop and report.
* **Gate G2 (M9)** — final performance gate vs. the Definition of Done (§15).

The most valuable single measurement in the project: **how fast does TF2 run when every D3D9 call is an instant no-op?** (`null` renderer, M3 builds it; Phase 0 gets a first answer from a 1-day throwaway.)

---------------------------------------------------------------------------
## 4. DECISIONS (locked; change only via `docs/decisions/ADR-*.md` with measurements)

| ID | Decision | Rationale / revisit trigger |
|---|---|---|
| D1 | **From scratch.** No code copied from dx9mt/DXMT/DXVK/MoltenVK/Wine. Specs used: Microsoft's public Direct3D 9.0c documentation (device, shader-model 2/3 instruction set, token encoding), Valve's Source SDK 2013 public source *for reading usage patterns only* (never vendored), Metal Shading Language spec, Wine's public unix-call/builtin-DLL design. DXVK (bundled in the runtime) is used **only as a black-box oracle** for pixel comparison and call-trace forwarding. | Owner mandate |
| D2 | **Single process, in-process backend.** x86_64 PE `d3d9.dll` (frontend, C) + x86_64 Mach-O `d3d9.so` (unix lib: backend + translator + Metal, C++20/ObjC++) connected by Wine **unix calls** (`__wine_unix_call`). The two share memory directly (same process): command ring and upload arenas are ordinary heap/`mmap` memory visible to both sides. | Simplest zero-copy design; shipped precedent F7. **Revisit (ADR) only if** the backend CPU on the render thread exceeds 1.2 ms at 2,500 draws (measured at M6) *and* a profile blames Rosetta-translated backend code. A native-arm64 helper process is the fallback and is **not** designed further now. |
| D3 | **Deployment as a Wine builtin replacement** in our private runtime: install `d3d9.dll` into `~/Games/tf2/wine/lib/wine/x86_64-windows/` (replacing Wine's own builtin `d3d9.dll`) and `d3d9.so` into `.../x86_64-unix/`; in the prefix, remove the native DXVK `system32/d3d9.dll` (move to `~/Games/tf2/backups/dxvk-d3d9/`) and let Wine materialize its builtin. `WINEDLLOVERRIDES=d3d9=b` for TF2. Rollback = restore backed-up files (script `scripts/layer-install.sh` / `layer-uninstall.sh`, M2). **Fallback if builtin marking proves impractical:** native PE beside `tf_win64.exe` + `MemoryWineLoadUnixLib`-style unix-lib loading (research item R2). | F7 shows the builtin+`.so` mechanism works; F11 says the "beside the exe" route was never actually tested with a correct-arch DLL |
| D4 | **Window/layer:** from our unix lib, find the `WineWindow` for the game's HWND and attach a `CAMetalLayer` — primary method: call Wine's own `macdrv_view_create_metal_view`/`macdrv_view_get_metal_layer` (reachable through `_macdrv_functions`/driver unix calls; **R1 research item** determines call mechanism); secondary: ObjC-runtime lookup of `WineContentView` and adding our own layer-backed child view. Never a separate window. | F6; FPS needs unified focus/mouse capture |
| D5 | **Shaders:** D3D9 bytecode → own IR → **MSL text** → runtime `newLibraryWithSource` on a compile pool; cache MSL + `MTLBinaryArchive` on disk. **Dynamic vertex pulling** (VS reads raw vertex streams; per-declaration decode tables in a constant block) so vertex declarations never multiply PSOs. **Function constants** specialise: alpha-test mode, clip-plane mask, fog mode, `b#`/`i#` values that Source treats as static, sRGB-write. AIR/metallib direct emission is a *later optimization* behind the same `Emitter` interface. | F8; MSL text is the only no-Xcode option |
| D6 | **Presentation:** `CAMetalLayer` (`BGRA8Unorm`, `framebufferOnly`, `maximumDrawableCount 3`, `displaySyncEnabled` from D3D present interval), pacing driven by `CAMetalDisplayLink` (macOS 14+) at the panel's 120 Hz; **2 frames in flight**; drawable acquired as late as possible by the present thread. | F1 |
| D7 | **Languages/build:** frontend = C17 (`x86_64-w64-mingw32-gcc`, no CRT heap calls in hot paths); unix lib/backend/translator = C++20 + ObjC++ built with Apple clang `-arch x86_64`; build system = plain `make` (no CMake). Tools (trace/replay) = C/C++; census/bench scripts = Python 3 in `.venv`. | no extra tooling needed |
| D8 | **Oracle & validation:** DXVK (from the runtime) is the pixel/behavior oracle through `tools/trace` + `tools/replay`. Divergences get a ruling in `docs/semantics.md`. | no native Windows D3D9 available |
| D9 | **Scope rule:** every implemented method/state/format/opcode must cite a row in `docs/census-report.md`. Everything else is a loud stub (log + `D3DERR_NOTAVAILABLE`/no-op as appropriate) that fails fast in debug builds. | owner rule (2) |
| D10 | **Latency/pacing target:** present interval stdev ≤ 0.5 ms at 120 Hz; frames in flight 2; never block the game thread except on the frame semaphore inside `Present`. | DoD |
| D11 | **No game memory access/hooks/injection** beyond being the D3D9 provider (VAC hygiene, §14). | safety |

---------------------------------------------------------------------------
## 5. PHASED PLAN OVERVIEW

| Phase | Name | Output | Gate |
|---|---|---|---|
| P0 | Baseline + ceiling | `docs/phase0-report.md` | **G0** |
| P1 | Trace & census | trace tool, census report, shader corpus | — |
| P2 | Wine integration spike | `d3d9.dll`+`d3d9.so` that clears TF2's window at 120 Hz | **G1** |
| P3 | Null renderer + replay harness | game-only ceiling confirmed; replay exe | — |
| P4 | Shader translator | corpus 100% translated+compiled | — |
| P5 | Resources/formats/locks | all census resources creatable, zero-copy dynamic ring | — |
| P6 | Core rendering | first correct frames (menu + harvest) | — |
| P7 | Full feature coverage | all §9 goldens pass; soak OK | — |
| P8 | Anti-stutter engine | cache, async PSO, pre-warm, pacing | — |
| P9 | Performance pass | ≥120 fps targets | **G2** |
| P10 | Hardening & packaging | install/uninstall, soak, docs | — |

Estimated effort for a strong model with full tool access: P0–P3 ≈ 2 weeks, P4–P6 ≈ 4 weeks, P7–P8 ≈ 3 weeks, P9–P10 ≈ 2 weeks. Highest variance: P4 (shader semantics), P7 (water/shadow/glow), P8 (hitch elimination).

---------------------------------------------------------------------------
## 6. PHASE 0 — baseline, ceiling, go/no-go (1–3 days)

**P0.1 Benchmark scenario.** In the TF2 environment record a demo on `koth_harvest_final` (`record bench`, ~3 min of real combat; `$TF2DIR/tf/bench.dem`) and replay in real time with `playdemo bench` (not `timedemo` — pacing is the point). Settings fixed: 1920×1080 borderless, `fps_max 0` for ceiling tests and `fps_max 120` for pacing tests, mastercomfig `medium`, owner's launch options. A fresh process per variant (F12); unique tag per run; ≥ 90 s capture after load settles; cvars logged.

**P0.2 Instrumentation (no PresentMon on macOS).** First tool = the **trace d3d9** from P1 in "timing-only" mode (cheap): timestamps every `Present` (`mach_absolute_time`), time in `Present`, draw/state counts, per-frame CPU time by thread (`thread_info`), writes CSV compatible with `tools/analysis/legacy_parallels_analyze.py` (port its percentile / 1%-low / spike / pacing logic). Also use `MTL_HUD_ENABLED=1` and `sample <pid>` (no sudo for own processes). No Instruments/xctrace (no Xcode).

**P0.3 Throwaway null renderer (≤ 1 day):** a minimal x86_64 PE `d3d9.dll` whose device accepts every call and draws nothing (enough for TF2 to reach a map: caps, formats, resource creation returning dummy memory for `Lock`) → **game-only ceiling**.

**P0.4 Tuned-DXVK baseline:** same stack as today, tuned: `DXVK_STATE_CACHE=1` + persistent cache path, `DXVK_CONFIG` (`d3d9.maxFrameLatency`, `d3d9.presentInterval`, `dxvk.enableAsync`-equivalent if present in 2.4.1), MoltenVK env (`MVK_CONFIG_*`), Wine sync mode (msync/esync if supported by this build), `fps_max 120`, mastercomfig preset sweep. Capture frame-time distributions and a **hitch list**.

**P0.5 Attribution + hitch taxonomy.** Per-frame split on the render thread: (i) TF2 game code, (ii) DXVK D3D9 front, (iii) DXVK→Vulkan, (iv) MoltenVK, (v) Metal driver, (vi) waits (`sample` call-graph sampling + trace timers). Classify every hitch > 2× median: PSO/shader compile, resource-creation burst, present/pacing, game thread, external. Count pipeline creations per hitch from DXVK logs.

**GATE G0 — continue only if ALL hold:**
1. Null-renderer game-only frame time ≤ **6.0 ms p99** (otherwise 120 fps is infeasible regardless of renderer → stop, report).
2. Tuned DXVK is **not** already ≥ 115 fps avg with p99 ≤ 10 ms and ≤ 3 hitches/min > 20 ms (otherwise ship the tuning, stop).
3. Translation-layer cost (items ii–v + pacing) ≥ **25 %** of render-thread time **or** ≥ 50 % of hitches > 20 ms are shader/PSO compile.
Write the numbers into `docs/phase0-report.md`.

---------------------------------------------------------------------------
## 7. PHASE 1 — TRACE & CENSUS (2–4 days)  ("only what TF2 requires", made measurable)

**Tool `tools/trace` → `d3d9.dll` (x86_64 PE):** exports exactly what shaderapi uses; loads the oracle `d3d9_ref.dll` (a copy of the runtime's DXVK `x86_64-windows/d3d9.dll`) and wraps **every** `IDirect3D9`/`IDirect3DDevice9`/resource/query/swapchain/state-block/shader/declaration method through vtable thunks **generated by a script from the method table in the D3D9 headers shipped with mingw-w64** (`/opt/homebrew/.../d3d9.h`). Logs method, thread, args, HRESULT; resource payloads hashed into a content-addressed zstd store (own tiny store or `zstd` CLI). Per-scenario `.t9` files (git-ignored, under `~/Games/tf2/cache/traces/`).

**Scenarios (≥ 5 min each):** main menu; every map type (`cp_`, `koth_harvest_final`, `pl_`, `ctf_`, `plr_`, arena, MvM); water maps; flashlight/projected textures; glow/outline; MSAA 0/2/4/8; `mat_hdr_level` 0/1/2; `-dxlevel` 90/95/98; windowed/borderless/resolution changes at runtime; alt-tab/focus loss; 50× `retry`/map change; spectator; demo playback; sprays/decals; particle-heavy scenes; HUD/vgui text; `screenshot`/`jpeg`; `mat_queue_mode 0` vs `2`.

**`tools/census` → `docs/census-report.md` (the authoritative scope), containing:**
* method × call-count × thread matrix;
* `CreateDevice` presentation parameters + behavior flags (`MULTITHREADED`? `FPU_PRESERVE`? `HARDWARE_VERTEXPROCESSING`? `PUREDEVICE`?) and whether `Direct3DCreate9Ex` is used (changes swap-chain semantics);
* every `D3DFORMAT × D3DRTYPE × D3DUSAGE × D3DPOOL` created or asked about via `CheckDeviceFormat`, with the answer Source expects (complete the FOURCC audit: `INTZ NULL ATI1N ATI2N ATOC DF16 DF24 RAWZ …`);
* every render/sampler/texture-stage state and value set (incl. `D3DRS_ADAPTIVETESS_*` as the ATOC switch, `SRGBWRITEENABLE`, `COLORWRITEENABLE1-3`, `SCISSORTESTENABLE`, `DEPTHBIAS`/`SLOPESCALEDEPTHBIAS`, all stencil states, `CLIPPLANEENABLE`, `MULTISAMPLEANTIALIAS`, fog, point size);
* primitive types, index formats, vertex-element types/usages, stream counts, `SetStreamSourceFreq` use;
* lock behavior per buffer class (static / dynamic `DISCARD` / `NOOVERWRITE` / `READONLY`), sizes, frequency → **sizes the dynamic ring**;
* queries used and polling pattern (`OCCLUSION`, `EVENT`; `GetData` flush behavior);
* **shader census:** unique VS/PS bytecode hashes per scenario, shader models, opcode histogram, register-file usage, constant ranges touched, `i#`/`b#` usage, flow control (`rep/loop/if/call`), sampler types, `vPos/vFace`, `texkill`, derivatives, `texldb/texldl/texldp`, declaration usages;
* draws/frame distribution (p50/p95/max) → sizes the command ring and removes any draw cap (**required capacity ≥ 2× observed max**);
* resource churn per map load, peak memory, texture upload volume per frame;
* the **adapter identity and caps** Source ends up accepting (see §8.1).

**VPK/VCS reader (`tools/census/vpk`):** read `hl2_misc_*.vpk` / `tf2_misc_*.vpk` directories, extract `shaders/fxc/*.vcs`, parse Valve's VCS container (header version, per-combo records, compressed bytecode) and emit the **superset corpus** (`~/Code/tf2mt/corpus/*.bin`, content-hashed). Used for translator regression and for AOT pre-warm manifests. (Container format reference: Source SDK 2013 `shaderlib`/`shaderapi` public sources.)

Acceptance: census reviewed; every later milestone cites census rows.

---------------------------------------------------------------------------
## 8. ARCHITECTURE

```
tf_win64.exe  (x86_64 PE under Rosetta; main thread + render thread when mat_queue_mode 2)
   │  COM vtable calls
   ▼
d3d9.dll  — x86_64 PE builtin (C).  Objects, caps, state tracker, packet encoder, dynamic-buffer arenas
   │  (A) lock-free SPSC command ring + upload arenas: plain memory shared with unix lib (same process)
   │  (B) rare control calls: __wine_unix_call (create device/layer, resize, resource heavy ops, shutdown)
   ▼
d3d9.so   — x86_64 Mach-O (C++20/ObjC++).  Encoder thread, translator + compile pool, PSO cache, heaps, present
   │
   ▼
Metal (native arm64 inside the OS)  →  CAMetalLayer on Wine's WineContentView  →  display @120 Hz
```

### 8.1 Frontend (PE) — what the API surface looks like
* Exports only: `Direct3DCreate9`, `Direct3DCreate9Ex` (only if census shows it), `D3DPERF_BeginEvent/EndEvent/SetMarker/SetRegion/QueryRepeatFrame/SetOptions/GetStatus` (no-ops).
* Hand-built COM vtables, intrusive refcounts, **32-bit generational handles** to backend objects (never raw pointers across the boundary).
* `IDirect3D9`: `GetAdapterIdentifier` returns an **engineered identity** (VendorId/DeviceId/driver strings) chosen so Source's `dxsupport.cfg` selects `dxlevel 98` (SM3.0, HDR, depth-texture shadows) and **avoids vendor workarounds we do not implement** — choose empirically in census (try an NVIDIA and an AMD row; read `mat_dxlevel` + `mat_hdr_level` + `r_` caps dump in the console log). `GetDeviceCaps`: table generated from the census (SM3.0, `MaxVertexShaderConst 256`, 4 simultaneous RTs with independent write masks/blend per census, two-sided stencil, depth bias, scissor, anisotropy 16, dynamic textures, automipgen, `MaxVertexIndex 0xFFFFFF`, cube/volume/mip, conditional NPOT). `CheckDeviceFormat/MultiSampleType/DepthStencilMatch/FormatConversion/EnumAdapterModes/GetAdapterDisplayMode`: **table-driven and honest** (Source branches on the answers).
* Device lifecycle: `CreateDevice`, `Reset` (resolution/MSAA/vsync/windowed↔borderless; no leaks), `TestCooperativeLevel` always `D3D_OK` (alt-tab never "loses" the device), `Present`. Fullscreen-exclusive requests become borderless at the monitor mode.
* Objects: `Texture9/CubeTexture9/VolumeTexture9` (volume needed for colour-correction LUTs), `Surface9`, `VertexBuffer9/IndexBuffer9`, `VertexDeclaration9`, `VertexShader9/PixelShader9`, `Query9`, `Swapchain9` (implicit only), `StateBlock9` **only if census shows use**.
* **Locking model (performance-critical):**
  * Dynamic VB/IB (`D3DUSAGE_DYNAMIC`): `Lock(DISCARD)` → new region of the **dynamic ring** (memory shared with the unix lib, zero-copy); `NOOVERWRITE` → append to the current region; `Unlock` publishes `[offset,len]`.
  * Static buffers/textures: host staging until `Unlock`, then one upload packet. Textures upload per mip with block-aware pitch. `MANAGED` pool: we never lose a device, so drop the CPU copy after upload unless census shows re-locks.
  * `LockRect`/readback on RTs/offscreen surfaces: `GetRenderTargetData` → blit into shared staging + fence; slow path (screenshots only).
* **State tracker:** mirrors exactly the D3D9 state TF2 sets. Dirty-bit groups: render states, sampler states (16 PS + 4 VS), textures, streams/indices/declaration, VS/PS float/int/bool constants (**range-dirtied**), viewport, scissor, clip planes, RT/DS bindings. A draw emits a ≤ 64-byte packet: draw args + dirty mask + inline deltas (render-state `(id,value)` pairs; constant ranges deduplicated against the previous draw by hash).
* Debug build validates arguments and **aborts with a precise message**; release trusts TF2.

### 8.2 Unix lib (backend) threads
Encoder thread (consumes ring in order) · compile pool (4 threads, QoS user-initiated: MSL translate + `newLibraryWithSource` + PSO creation) · present thread driven by `CAMetalDisplayLink` · cache I/O thread. 2 frames in flight; backpressure only inside `Present`.

### 8.3 Formats (Metal mapping; finalize from census)
| D3D9 | Metal | Notes |
|---|---|---|
| A8R8G8B8 / X8R8G8B8 | `BGRA8Unorm` (+`_sRGB` view for sRGB read/write) | X8: force alpha 1 on sample |
| DXT1/3/5 | BC1/2/3 | block pitch, mip tails, 1-bit alpha |
| ATI1N / ATI2N | BC4 / BC5 | normal-map channel convention |
| A16B16G16R16F | `RGBA16Float` | HDR RTs; must be blendable |
| A16B16G16R16 / R32F / A32B32G32R32F | `RGBA16Unorm` / `R32Float` / `RGBA32Float` | check 32-bit-float filtering support at startup |
| L8, A8, A8L8, V8U8, Q8W8V8U8, X8L8V8U8, CxV8U8 | `R8Unorm`, `R8Unorm`(swizzle), `RG8Unorm`, `RG8Snorm`, `RGBA8Snorm`, … | bump-map formats; signedness emulation in shader where needed |
| A4R4G4B4, A1R5G5B5, R5G6B5 | `ABGR4Unorm`, `A1BGR5Unorm`, `B5G6R5Unorm` | rare; census |
| D24S8 / D24X8 / D16 / D32F | `Depth32Float_Stencil8` / `Depth32Float` / `Depth16Unorm` (no D24 on Apple GPUs) | **depth-bias scale follows the real format** |
| INTZ / DF16 / DF24 / RAWZ (depth-as-texture) | sampleable `Depth32Float(+Stencil8)` | sampling with comparison = hardware PCF: use `compare_func=less_equal`, linear filter when bound as shadow sampler |
| `NULL` FOURCC RT | no attachment (depth-only pass) | no storage |
| ATOC (via `D3DRS_ADAPTIVETESS_Y`) | `alphaToCoverageEnabled` | meaningful with MSAA only |
| MSAA RT + `StretchRect` resolve | `MultisampleResolve` store action / resolve pass | |

### 8.4 Shader translation (the hard core)
Pipeline: **parse** (token stream, operand-length self-check) → **IR** (SSA-ish typed vec4, explicit swizzle/write-mask nodes, predication, structured control flow incl. `rep/loop/if/call`) → **specialisation** by function constants (alpha-test mode+ref compare, clip-plane mask, fog mode, sRGB-write, `b#`, `i#`) → **MSL emitter** (`Emitter` interface; AIR emitter later) → **cache** keyed `xxh3_128(bytecode) ⊕ translator_version ⊕ specialization` (own xxh3-compatible hash implementation or SipHash; no third-party code) at `~/Games/tf2/cache/shaders/`.

Exact D3D9 semantics (each has a unit test against a CPU reference interpreter, written from the Microsoft SM2/3 spec): `rcp/rsq` of 0 → +INF; `pow` = `exp2(y·log2(|x|))`; `lit`, `dst`, `nrm` (zero length → 0), `sincos` range, `cmp`, `lrp`, `mad` without cross-instruction fusion that changes results, `min/max` NaN behavior, `dp2add`, `m3x3…m4x4` macros, `texkill`, `dsx/dsy`, `texld/texldp/texldb/texldl/texldd`, saturate/shift/partial-precision, all source modifiers, `vPos` (integer-centre convention), `vFace` sign, `oDepth`, `oC0-3`, `oPos`, `oFog`, `oPts`, `oT#`, `oD#`. **Half-pixel offset** applied in every VS epilogue (`pos.xy += (-1/vp.w, +1/vp.h)·pos.w`; TF2's HUD/post-process relies on it). Clip planes via `[[clip_distance]]` from the 6 user planes (specialisation key). Alpha test emulated at PS epilogue with `discard_fragment`.

**Dynamic vertex pulling:** VS receives raw stream buffers + a per-declaration decode table (offset, stride, format id) in a constant block; the MSL prologue decodes `FLOAT1-4`, `D3DCOLOR` (BGRA→RGBA), `UBYTE4/UBYTE4N`, `SHORT2/4(N)`, `USHORT2N/4N`, `UDEC3/DEC3N`, `FLOAT16_2/4` (the compressed formats Source uses for models). **Constants:** VS 256×vec4 and PS 224×vec4 float constants + `i#`/`b#` via a per-frame **constant ring**, only dirty ranges copied, offsets 256-byte aligned; `setVertexBytes` only for ≤ 4 KB tiny draws.

### 8.5 Render-pass planning (TBDR-aware)
One `MTLRenderCommandEncoder` per contiguous run of draws to the same RT/DS set. **Merge** `Clear` issued before a pass's first draw into the load action. Per-attachment "contents needed" tracking: `DontCare` loads when fully overwritten/cleared; `DontCare` depth/stencil stores unless the surface is later sampled (flag set at creation for INTZ-style depth textures); MSAA store = resolve-only if the MS texture is never read. `StretchRect` → blit (same format) / resolve / tiny internal pass (scale/convert/filter). `ColorFill` → clear pass. `UpdateSurface/UpdateTexture` → blit from staging. Full stencil incl. two-sided; `DEPTHBIAS/SLOPESCALEDEPTHBIAS` → `setDepthBias` with per-format scale; `MTLDepthStencilState` cache. **PSO key** = (VS id+spec, PS id+spec, up to 4 colour formats, DS format, sample count, blend ×4 incl. separate alpha + write masks, ATOC, topology class). Triangle fans re-indexed to lists in the backend (cache by index range). Points/lines only if census shows use.

**PSO-miss policy (anti-stutter core):** pre-warm manifest at map load → background compile on miss → while pending, *skip non-critical draws* (particles/decals) for ≤ N frames, *wait ≤ 8 ms* for critical classes (world/models) → encoder never blocks > 1 ms in steady state → every miss logged to rebuild the manifest (`tools/census/warm`).

### 8.6 Presentation & input
`CAMetalLayer` attached per D4 inside the game window's content view; drawable acquired late; `CAMetalDisplayLink` supplies the real cadence; `Present` interval 1 ⇒ each vblank, `fps_max 120` ⇒ 1:1 on the 120 Hz panel. Do **not** add a second gamma transfer (Source applies `mat_monitorgamma` in its own shaders). Max 2 frames in flight; `TF2MT_MAX_LATENCY` env for experiments. Mouse capture/focus stay with Wine's window (we never create a window).

### 8.7 Queries / sync / readback
`OCCLUSION` → Metal visibility result buffer (boolean/counting), results ≥ 1 frame later, `GetData` returns `S_FALSE` until ready, `D3DGETDATA_FLUSH` commits the command buffer. `EVENT` → shared event/fence at command-buffer completion. Hazards: tracked resources by default; manual fences for heap-aliased transient targets; ring regions fenced per frame.

---------------------------------------------------------------------------
## 9. TF2 SEMANTIC CHECKLIST (every item becomes a golden test)
1. Half-pixel offset in VS (HUD, post-process, `StretchRect` filtering). 2. sRGB read/write per sampler/per draw; HDR RT + tonemap; blending on RGBA16F. 3. Shadow maps: depth-texture sampling + hardware PCF; `NULL` RT depth-only passes; depth bias per format. 4. Projected textures (`texldp` with `w ≤ 0`). 5. Water: refraction/reflection RTs, oblique clip plane, backbuffer→texture `StretchRect`, cheap-water path. 6. Alpha test + ATOC with MSAA; decals with depth bias; particle blend combos. 7. Normal-map formats (BC5/`V8U8`/`Q8W8V8U8`), `$ssbump`. 8. Stencil: glow/outline pipeline, any portal-style masks; two-sided stencil if used. 9. Sampler address modes incl. **border** (Metal: limited border colours — emulate other colours in PS if census shows them), mirror-once, anisotropy 1–16, LOD bias, `MAXMIPLEVEL`, mip filter `NONE`. 10. Vertex texture fetch (VS samplers) if census shows use. 11. `Clear` with rect lists + scissor, combined `TARGET|ZBUFFER|STENCIL`. 12. Viewport `MinZ/MaxZ ≠ 0..1` (viewmodel depth tricks) → `MTLViewport` znear/zfar. 13. Scissor toggling per draw. 14. `GetBackBuffer`/`GetRenderTarget(0)` identity/format; `Present` with rects only if census shows. 15. `CheckDeviceFormat` honesty. 16. `Reset` correctness (no leaks); `GetAvailableTextureMem` plausible (unified memory → report a sensible recommended working set). 17. Threading semantics if `MULTITHREADED` / `mat_queue_mode 2`. 18. Screenshots via `GetRenderTargetData` + `LockRect`.

---------------------------------------------------------------------------
## 10. MILESTONES & ACCEPTANCE (do not start N+1 before N passes)

| M | Deliverable | Acceptance test |
|---|---|---|
| **M0** | Phase 0 (§6) | G0 passed and written down; else stop |
| **M1** | `tools/trace` + census + VPK/VCS reader | `docs/census-report.md` complete; ≥ 8 scenario traces; corpus extracted |
| **M2** | Wine integration spike (**G1**): research **R1** (CAMetalLayer acquisition: `_macdrv_functions` / driver unix calls vs ObjC runtime) and **R2** (builtin deployment D3 vs native-PE fallback); `d3d9.dll`+`d3d9.so` create a device, clear TF2's window to a colour, present at 120 Hz; `scripts/layer-install.sh` / `layer-uninstall.sh` | TF2 shows the coloured window, input works, ≥ 119.5 fps average presented over 60 s, uninstall restores DXVK; ADR-002 records the mechanisms |
| **M3** | Null renderer (all calls accepted, nothing drawn) + `tools/replay` | TF2 reaches a map and plays; frame time within ±5 % of Phase-0 null ceiling |
| **M4** | Shader translator v1 | Unit/golden opcode tests green; **100 %** of census + VPK corpus translates and Metal-compiles |
| **M5** | Resources/formats/locks (§8.1, §8.3) | Replay of 3 scenarios creates every resource without error; upload bandwidth within ledger targets |
| **M6** | Core rendering: passes, PSO cache (sync compile), state mapping, draws, `Clear`, `StretchRect`, depth/stencil/blend/sampler | **First correct frames**: main menu + `koth_harvest_final` world geometry, SSIM ≥ 0.98 vs DXVK oracle |
| **M7** | Full §9 coverage | All goldens SSIM ≥ 0.995 (documented FP exceptions); 50× map-change soak without leak/crash; `Reset` tests |
| **M8** | Anti-stutter engine (§8.5 policy, binary archive, pre-warm manifest, persistent cache, pacing) | Cold cache: ≤ 3 hitches/min > 20 ms after load settles; warm: ≤ 1/min; present-interval stdev ≤ 0.5 ms |
| **M9** | Performance pass (**G2**): argument buffers, constant dedup, index-scan cache, batching, thread tuning, optional AIR emitter | §15 targets met on the demo; layer CPU ≤ 2 ms/frame, GPU ≤ 4 ms; else documented ceiling and stop |
| **M10** | Hardening + packaging | 2-hour online soak (owner-run), crash handling, logs, one-command install/uninstall, README |

---------------------------------------------------------------------------
## 11. VERIFICATION & MEASUREMENT
* **Oracle = DXVK** via `tools/trace` → `.t9` → `tools/replay` against ours vs DXVK; SSIM ≥ 0.995 per golden, per-pixel max-error thresholds, documented FP exceptions; DXVK-vs-Valve-intent rulings in `docs/semantics.md`.
* **Perf ledger** emitted by the layer (per second and per 120 frames): frame ms p50/p99/max, game-thread ms between presents, frontend ms/frame, backend encode ms, GPU ms (`MTLCommandBuffer.GPUStartTime/GPUEndTime`), draws, state changes, PSO misses + compile ms, bytes uploaded, ring high-water, present-interval stats, late/dropped frames.
* **A/B discipline** (lessons from the earlier session): fresh process per variant; unique tags (result folders created by other processes may be undeletable — never reuse a tag); distributions not averages; record all cvars; raw data kept; never trust a result whose process restart wasn't verified.
* **Soak/fuzz:** replay with randomized resize/`Reset`; 1000× device create/destroy; translator fuzz (mutated bytecode must fail cleanly).
* **Safety rail:** `TF2MT_FALLBACK=dxvk` (env) makes the install fall back to the oracle so the owner can always play; `layer-uninstall.sh` restores the original DXVK DLL.

---------------------------------------------------------------------------
## 12. RISKS
| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Game-only cost on Rosetta already > 8.3 ms | Med–High | project value collapses | G0 null ceiling first; report; separate track to cut game CPU via cvars |
| Wine integration impossible without patching Wine (R1/R2) | Medium | M2 blocked | try builtin-replacement, then native-PE+unixlib-load, then ObjC-runtime view injection; last resort tiny Wine patch (rebuild out of scope → stop and report) |
| Backend under Rosetta too slow (D2) | Medium | CPU budget | M6 measurement; ADR to move hot parts to arm64 helper |
| MSL runtime compile hitches dominate | High | stutter persists | cache + async + pre-warm + specialisation; AIR emitter in M9 |
| Shader semantic bugs | High | corruption | CPU reference interpreter + per-opcode goldens + corpus diff |
| Vendor-specific hacks (INTZ, NULL RT, ATOC, DF24, RESZ) | Medium | missing effects | census FOURCC audit; honest `CheckDeviceFormat` |
| Apple-GPU format/feature gaps (no D24, 32F filtering) | Medium | wrong shadows/post | format table + runtime feature queries; emulate |
| No Xcode (no GPU capture/Instruments) | Certain | harder debugging | own ledger, Metal HUD, `sample`; **ask owner before** installing Xcode |
| TF2 update changes D3D usage | Low–Med | regressions | pin buildid 25738890 in docs; rerun census/goldens after updates |
| VAC / online-integrity concerns for a replaced renderer DLL | Must verify | account risk | see §14 |
| Prefix corruption during experiments | Low | lose Steam/TF2 | `scripts/backup-env.sh` before risky steps |

---------------------------------------------------------------------------
## 13. BUILD & RUN COMMANDS (to be created in M2; shown for orientation)
```sh
cd ~/Code/tf2mt
make frontend      # x86_64-w64-mingw32-gcc → build/d3d9.dll
make unixlib       # clang -arch x86_64 -std=c++20 -fobjc-arc … → build/d3d9.so
make tools         # trace, replay, census
make install       # scripts/layer-install.sh (backs up DXVK d3d9, installs ours into ~/Games/tf2/wine/…)
scripts/steam.sh && scripts/tf2.sh          # run
scripts/stop.sh                              # stop everything
make uninstall     # scripts/layer-uninstall.sh (restore DXVK)
make test          # unit + golden
```
Python env: `python3 -m venv ~/Code/tf2mt/.venv && .venv/bin/pip install numpy pandas scikit-image pillow zstandard`.

---------------------------------------------------------------------------
## 14. GUARDRAILS FOR THE EXECUTING MODEL
1. **Order is law:** P0 before anything else; never skip a gate; write every measurement into `docs/`.
2. **Back up first:** run `scripts/backup-env.sh` before touching the prefix/runtime (the folder is the only copy of Steam + TF2).
3. **Ask the owner only for:** installing Xcode; anything involving their online account/servers beyond ordinary play-testing; deleting their data. Everything else, proceed.
4. **VAC hygiene (do before any online testing):** tf2mt is *only* a D3D9 provider. It must contain no game-memory reads/writes, no hooks, no injection, no overlays reading game state, and must never be described or built as a cheat aid. Verify (read-only) whether TF2 checks `d3d9.dll` integrity on secure servers; community practice (Wine/Proton/DXVK users) suggests a replaced D3D9 DLL is tolerated, but treat as **unverified** and tell the owner before first online session. Offline/bot/demo benchmarks first.
5. **From-scratch rule:** do not copy code from dx9mt, DXMT, DXVK, MoltenVK, Wine, or Valve's SDK; reading Microsoft/Apple/Valve public documentation and observing behavior through traces is fine; keep `THIRD_PARTY.md` empty unless something is knowingly adapted (then record provenance).
6. **Known gotchas:** FreeType warning means `DYLD_FALLBACK_LIBRARY_PATH` is unset (use `env.sh`); `tf.exe` re-execs `tf_win64.exe`; cvars forwarded to a running game are ignored (F12); result directories created by foreign processes may be undeletable (use fresh tags); `Direct3DCreate9Ex` presence needs a census answer before designing `Present`.
7. Keep a running `docs/decisions/ADR-*.md` log; each scope cut cites a census row.

---------------------------------------------------------------------------
## 15. DEFINITION OF DONE
TF2 (64-bit, current Steam build) launched via `scripts/tf2.sh` on this Mac with `tf2mt` installed: **≥ 120 fps average on `koth_harvest_final` (online and demo playback), 1% low ≥ 100 fps, p99.9 ≤ 12 ms, ≤ 1 hitch > 20 ms per minute after warm-up, presented-interval stdev ≤ 0.5 ms**; golden set at SSIM ≥ 0.995; 2-hour soak with no crash; one-command install/uninstall that restores DXVK; README with limits. If G0 or G2 proves this unattainable, the deliverable is a **written ceiling analysis with data** plus the tuned-DXVK configuration — and the owner is told plainly.
