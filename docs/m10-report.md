# M10 report — hardening and packaging

Date: 2026-10-08. Gate (PLAN §10): *2-hour online soak (owner-run), crash handling, logs, one-command
install/uninstall, README.* **Status: done except the online soak, which is the owner's to run** (VAC hygiene,
PLAN §14.4: never tested online by the agent).

## Delivered
| Item | How |
|---|---|
| **Renderer choice** | `TF2_RENDERER=tf2mt scripts/tf2.sh …` or the launcher option **Native Metal renderer (experimental)**, off by default. DXVK stays the default renderer. |
| **One-command install / uninstall** | Per session, automatically: `tf2.sh` installs the layer just before launch and removes it when TF2 exits. Manual: `scripts/layer-install.sh` / `scripts/layer-uninstall.sh`. |
| **Crash handling** | The uninstall watcher waits on the TF2 process, not on a clean quit, so a crash also restores DXVK. If the layer can't be installed (files missing), `tf2.sh` says so and starts on DXVK. |
| **Logs** | Each session's renderer logs (`unix.log`: present cadence, render/pipeline/upload/perf ledgers, stall log; `tf2mt-session.log`: frontend) in `~/Games/tf2/logs/tf2mt/<timestamp>/`; the last 5 sessions are kept. |
| **VAC safety default** | tf2mt sessions add `-insecure` (no VAC-secured servers) unless `TF2MT_ALLOW_SECURE=1` is set deliberately. |
| **Packaging** | `tools/launcher/build.sh` bundles the prebuilt layer (`tf2mt.dll`, `tf2mt.so`, `d3d9.dll`) into `tf2mt.app` and `tf2mt-app.zip`. The new app is installed in `/Applications`; the bundled install/uninstall was tested. |
| **README** | "Experimental: the native Metal renderer" section, launcher option, repository map. |

## Verified tonight
* `TF2_RENDERER=tf2mt scripts/tf2.sh -insecure … +map koth_harvest_final`: renderer active and `-insecure`
  applied. 120.0 fps, pacing sd 0.12 ms. After `quit`, the layer was gone, the game folder clean and `config.cfg`
  unchanged.
* Cold-cache session with 22 bots: renderer stalls 0. Layer removed after exit.
* Bundled copy in `/Applications/tf2mt.app`: install and uninstall OK.

## For the owner (not done by the agent)
1. **Decide on online use.** VAC tolerance of a replaced `d3d9.dll` / Wine builtin is unverified. Community
   practice (Wine/Proton/DXVK users) suggests it is tolerated, but tf2mt is new code. To play online with tf2mt,
   set `TF2MT_ALLOW_SECURE=1` (or remove `-insecure` handling) knowingly.
2. **2-hour online soak:** play normally with the renderer on for 2 h and check
   `~/Games/tf2/logs/tf2mt/<session>/unix.log`:
   * `grep '^present:'`: sd ≤ 0.5 ms;
   * `grep -c '^stall'`: expect 0;
   * `grep 'render:'`: 0 skipped.
   This also gives the online M8/M9 hitch numbers (no local bot simulation).
3. **Disk space:** captures from M5–M7 take ~43 GB in `~/Games/tf2/cache/traces/`. Keep them for regression
   (`tools/replay/goldens.sh`) or delete them; `tests/golden/frames.tsv` lists the ones the golden set uses. Metal
   compile caches moved aside for the cold-cache tests are in `~/Games/tf2/cache/metal-cache-aside*` (safe to
   delete).

## First online session (owner, 2026-10-08): missing no-mip textures — fixed
**Symptom:** with the Metal renderer, the sky, crosshair, UI button art, menu background, class portraits,
backpack/item icons and avatars were missing.

**Cause:** D3D9Ex *user-memory* system textures. For `D3DPOOL_SYSTEMMEM`, TF2 passes a pointer to its own pixel
memory in `pSharedHandle`, writes pixels there without `LockRect`, then calls `UpdateSurface`. It does this for
every texture without mipmaps (in the loadout capture, 4,643 of 5,130 `UpdateSurface` sources were never locked).
tf2mt ignored the pointer and silently uploaded nothing. The capture tool ignored it too, so the reference
recordings also lacked these images on DXVK, and the golden tests could not see the gap.

**Fix:**
* `src/frontend/device.c` `use_user_memory()`: a SYSTEMMEM texture/offscreen surface created with
  `*pSharedHandle` uses that memory as its level 0 (4-byte-aligned pitch for uncompressed formats).
* `tools/trace/capture.c`: records the user memory's contents (TEXTURE_WRITE/SURFACE_WRITE) right before each
  `UpdateSurface`/`UpdateTexture` that reads it, so new captures and goldens include these images.

**Verified live:** the main menu now shows background art, character model, logo, icons, avatars, featured item
and XP bar. The rebuilt launcher is installed. Existing captures still lack these images; recapture before using
them as references for UI/sky.

## Finding (2026-10-08): `-insecure` does not reach the game
`scripts/tf2.sh` passes `-insecure` to `tf.exe`, but the running `tf_win64.exe` command line does not contain it:
all other arguments arrive, this one is stripped on the way (observed with `ps` on a TF2MT_INSECURE=1 launch).
Consequently the agent's earlier "offline" test launches (census, captures, soak, smoke, bench) most likely ran in
normal VAC-secure mode. They only connected to local listen servers or played demos, and the non-local-connection
guard in the runners stayed in place, but the claim that tests ran `-insecure` was wrong. If insecure mode is
needed in future, launch `tf_win64.exe` directly or verify the flag in the process arguments.
