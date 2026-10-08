# tf2mt — Team Fortress 2 tuned for Apple Silicon

<img width="1092" height="834" alt="Screenshot 2026-10-08 at 13 12 27" src="https://github.com/user-attachments/assets/a286a706-6c99-44f1-b56a-120eae0c0b9c" />

tf2mt runs the 64-bit Windows version of **Team Fortress 2** on Apple Silicon Macs under Wine 10, with its own
Direct3D 9 → Metal renderer (or DXVK + MoltenVK), a set of fixes that were each **measured** on an M1 Max at
120 Hz, and a native macOS launcher.

> Status: playable online (VAC-secured servers). The native Metal renderer ([PLAN.md](PLAN.md), milestones M0–M10)
> is the default; DXVK remains one switch away. Current state: [docs/STATUS.md](docs/STATUS.md).

## What tf2mt fixes

| Problem on a stock Wine setup | tf2mt fix | Measured result |
|---|---|---|
| Mouse turning feels like 30–40 fps at a steady 120 fps | Patch in Wine's macOS driver: stop discarding mouse motion on every cursor warp (TF2 re-centres the cursor each frame) | Camera updates while turning: **~40/s → ~120/s** ([docs/mouse-input.md](docs/mouse-input.md)) |
| Aim feels sluggish at a steady 120 fps (Metal renderer) | Shallower frame queue: the game no longer runs a frame ahead of the renderer, 2 display buffers instead of 3 | Frame start → on screen: **~41 ms → ~25–31 ms** ([docs/mouse-input.md](docs/mouse-input.md)) |
| 2–4 fps in the loadout/class-select screens | Steam Friends set offline while playing (TF2's friends panel issues ~1000 Steam IPC calls per frame on each friend status change; each is ~1 ms under Wine) | Storm seconds: **39 → 0** ([docs/loadout-stall.md](docs/loadout-stall.md)) |
| Shader-compile hitches | DXVK async pipeline compiling + state cache | Hitches > 20 ms: **7.1 → 3.7 /min** |
| Tearing | Tear-free presentation (vsync, triple buffered; MoltenVK has no mailbox mode) | No tearing at the display's refresh rate |
| TF2 crashes during map load when started too early | Launch waits until Steam has logged in | — |
| Things that *look* like tuning but hurt (msync, frame-latency 1, `fps_max` caps) | Left off by default, documented | see [docs/phase0-report.md](docs/phase0-report.md) |

## Requirements

* Apple Silicon Mac, macOS 13 or newer (developed on macOS 27, M1 Max)
* ~35 GB free disk (TF2 ≈ 31 GB)
* A Steam account (TF2 is free)
* Nothing else: no Xcode, developer tools or Homebrew. Setup uses only what ships with macOS.

## Install

1. Download **`tf2mt-app.zip`** from the [releases](../../releases) page, unzip it and move **tf2mt.app** to Applications.
   The app is ad-hoc signed, so on first launch right-click it → **Open**.
2. Open tf2mt → **Setup** tab → **Run setup**. It installs, into `~/Games/tf2` (changeable):
   Rosetta 2 (if missing) · the tf2mt Wine runtime (~230 MB download) · a Wine prefix with DXVK · Steam for Windows
   (official Valve installer) · tf2mt's configs · the mouse fix.
   The first **Play** also installs [mastercomfig](https://github.com/mastercomfig/mastercomfig) with the Low preset.
3. Click **Log in to Steam** and sign in once (Steam Guard works as usual).
4. Click **Install TF2** and let Steam download it.
5. **Play.**

### Updates

The launcher checks GitHub for a new release on launch and every 6 hours (**Setup** tab → *Check for updates
automatically*, or **Check now**). If there is one, **Update now** downloads `tf2mt-app.zip`, verifies it against
the SHA-256 checksum GitHub publishes for the file, checks the app's identity and signature, swaps it in place and
relaunches. It won't update while TF2 is running. Your game folder, settings and logs are untouched.
(Updating from v0.2.0 or older: download v0.3.0 once by hand. The updater ships from v0.3.0 on.)

Everything can also be done from a terminal with the same scripts:

```sh
git clone https://github.com/HorrorPills/tf2mt ~/Code/tf2mt && cd ~/Code/tf2mt
scripts/setup.sh                 # or: scripts/setup.sh --runtime ~/Downloads/tf2mt-runtime-*.tar.xz
scripts/steam.sh --login         # first login
scripts/install-tf2.sh           # opens Steam's install dialog for TF2
scripts/play.sh                  # Steam (if needed) + TF2
```

Set `TF2_HOME` to use a different game folder (default `~/Games/tf2`).

## Launcher options

**Settings** (defaults in bold):

* **Native Metal Renderer:** **on**. tf2mt's own Direct3D 9 → Metal renderer; off uses DXVK + MoltenVK.
* **Graphics preset:** **Low (recommended, competitive)**, Medium, High or Ultra, from
  [mastercomfig](https://github.com/mastercomfig/mastercomfig). Changes only the `preset=` line of mastercomfig's
  `cfg/app/setup_hook.cfg`, so your addons stay. Applies on the next launch
  (`scripts/mastercomfig.sh status|set <preset>`).
* **Frame pacing:** **vsync** at your display's refresh rate, uncapped, or an fps cap (caps use TF2's own limiter,
  which paces unevenly under Wine).
* **Smooth mouse fix:** **on**. Aim updates on every rendered frame, at any refresh rate.
* **Friends offline while playing:** **on**. Prevents menu stutter; turn off if you need party invites.
* **Extra launch options:** passed to TF2 like Steam launch options.

**Debug:** Metal performance HUD (Apple's frame-rate overlay) and frame-time recording for DXVK sessions
(`~/Games/tf2/logs/dxvk/`; the Metal renderer always records to `~/Games/tf2/logs/tf2mt/`).

Starting TF2 from the Steam window bypasses the launcher's options. Use tf2mt to play.

## The native Metal renderer

tf2mt includes its own Direct3D 9 → Metal renderer, built only for TF2 (no DXVK, MoltenVK or Vulkan in between).
It is on by default in the launcher (**Native Metal Renderer**); from a terminal: `TF2_RENDERER=tf2mt scripts/tf2.sh`.

* **Status:** renders TF2 to within SSIM ≥ 0.999 of DXVK on every reference scene: menus, loadout, HDR, MSAA,
  high settings, resolution changes. On the benchmark demo it is ~15 % faster than DXVK on Low; on Medium its
  average is ~22 % lower but its frame times are steadier (1 % low 118 vs 60 fps, uncapped). Shaders compile
  in the background and are pre-warmed from a persistent cache (`~/Games/tf2/cache/tf2mt`). Details: `docs/m6-report.md` … `docs/m9-report.md`.
* **Online:** plays on VAC-secured servers (Casual, community).
* **Nothing permanent:** the renderer is installed when TF2 starts and removed when it exits (also after a crash).
  Turning the option off plays on DXVK. Session logs: `~/Games/tf2/logs/tf2mt/` (last 5).
* **Known limits:** tested on Low and Medium (including online play) plus HDR, MSAA and high-detail captures. MvM,
  `mat_queue_mode 2` and flashlight-heavy scenes are untested. Fixed-function draws are minimal: TF2 only uses
  them for depth passes.

## Good to know

* **Nothing keeps running after you're done.** When TF2 exits (however it is closed) Steam and Wine are shut down cleanly, and leftovers from crashes are cleared on the next start. Closing the launcher never interrupts a running game ([docs/session-lifetime.md](docs/session-lifetime.md)).
* **VAC / online play:** tf2mt does not touch the game or its memory. It is a Wine configuration, like Proton on
  Linux/Steam Deck. The Wine patch changes Wine's mouse handling, not TF2.
* **Nothing from Valve is redistributed.** Steam and TF2 are downloaded from Valve into your folder. The launcher
  shows TF2 art and fonts by reading them from your own installation.
* The mouse fix only applies to the exact Wine build shipped as the tf2mt runtime (the patch script verifies the
  bytes and refuses otherwise).
* Logs: `~/Games/tf2/logs` (launcher **Logs** button).

## Repository

| Path | What |
|---|---|
| `scripts/` | setup, Steam/TF2 launch, benchmarking (`bench.sh`), runtime packaging |
| `config/` | DXVK configs, `release.conf` (runtime download URL + checksum) |
| `tools/launcher/` | SwiftUI launcher (`build.sh` builds `tf2mt.app` and `tf2mt-app.zip`) |
| `tools/wine-patches/` | the winemac mouse patch (`apply` / `revert` / `status`) |
| `tools/trace`, `tools/null`, `tools/bench/` | measurement tools: D3D9 timing proxy, null renderer, analysis |
| `src/` | the native renderer: `frontend/` (PE d3d9 replacement), `unixlib/` (Metal backend), `translate/` (D3D9 shader → MSL) |
| `tools/replay/` | capture/replay of TF2's D3D9 call stream, golden-frame and soak tests |
| `tests/` | translator opcode tests, golden frame list |
| `docs/` | measurements and investigations (`STATUS.md` = current state; `m*-report.md` = renderer milestones) |

## Publishing a release (maintainers)

1. `scripts/package-runtime.sh` → `build/release/tf2mt-runtime-*.tar.xz` (+ `.sha256`) — the runtime with the
   original, unpatched `winemac.so`.
2. Commit, then tag the release (`git tag vX.Y.Z`): the app's version comes from `git describe --tags`, and the
   in-app updater compares it with the release tag. Then `tools/launcher/build.sh --no-install` →
   `build/launcher/tf2mt-app.zip`.
3. Create a GitHub release for the tag and upload the files (the runtime only when it changed). The updater only
   offers non-draft, non-prerelease releases whose tag is newer than the installed app and that carry an asset
   named exactly `tf2mt-app.zip`.
4. Put the runtime's download URL and SHA-256 into `config/release.conf`, commit, and rebuild the app (step 2) so the
   bundled setup downloads the runtime automatically.

See [THIRD_PARTY.md](THIRD_PARTY.md) for the components in the runtime and their licences.
