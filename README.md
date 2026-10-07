# tf2mt — Team Fortress 2 tuned for Apple Silicon

tf2mt runs the 64-bit Windows version of **Team Fortress 2** on Apple Silicon Macs (Wine 10 + DXVK + MoltenVK
→ Metal), with a set of fixes that were each **measured** on an M1 Max at 120 Hz, and a native macOS launcher.

> Status: playable and tuned. The long-term goal in [PLAN.md](PLAN.md) — a TF2-specific D3D9 → Metal renderer —
> is in progress (Phase 0 done, see [docs/phase0-report.md](docs/phase0-report.md)). Today's builds use DXVK.

## What tf2mt fixes

| Problem on a stock Wine setup | tf2mt fix | Measured result |
|---|---|---|
| Mouse turning feels like 30–40 fps at a steady 120 fps | Patch in Wine's macOS driver: stop discarding mouse motion on every cursor warp (TF2 re-centres the cursor each frame) | Camera updates while turning: **~40/s → ~120/s** ([docs/mouse-input.md](docs/mouse-input.md)) |
| 2–4 fps in the loadout/class-select screens | Steam Friends set offline while playing (TF2's friends panel issues ~1000 Steam IPC calls per frame on each friend status change; each is ~1 ms under Wine) | Storm seconds: **39 → 0** ([docs/loadout-stall.md](docs/loadout-stall.md)) |
| Shader-compile hitches | DXVK async pipeline compiling + state cache | Hitches > 20 ms: **7.1 → 3.7 /min** |
| Tearing | Tear-free presentation (vsync, triple buffered; MoltenVK has no mailbox mode) | No tearing at the display's refresh rate |
| TF2 crashes during map load when started too early | Launch waits until Steam has logged in | — |
| Things that *look* like tuning but hurt (msync, frame-latency 1, `fps_max` caps) | Left off by default, documented | see [docs/phase0-report.md](docs/phase0-report.md) |

## Requirements

* Apple Silicon Mac, macOS 13 or newer (developed on macOS 27, M1 Max)
* ~35 GB free disk (TF2 ≈ 31 GB)
* A Steam account (TF2 is free)

## Install

1. Download **`tf2mt-app.zip`** from the [releases](../../releases) page, unzip it and move **tf2mt.app** to Applications.
   The app is ad-hoc signed, so on first launch right-click it → **Open**.
2. Open tf2mt → **Setup** tab → **Run setup**. It installs, into `~/Games/tf2` (changeable):
   Rosetta 2 (if missing) · the tf2mt Wine runtime (~230 MB download) · a Wine prefix with DXVK · Steam for Windows
   (official Valve installer) · tf2mt's configs · the mouse fix.
3. Click **Log in to Steam** and sign in once (Steam Guard works as usual).
4. Click **Install TF2** and let Steam download it.
5. **Play.**

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

* **Frame pacing:** vsync matched to your display (any refresh rate), uncapped, or an fps cap
  (caps use TF2's own limiter, which paces unevenly under Wine; vsync is smoother).
* **Metal performance HUD:** Apple's overlay with the real displayed frame rate.
* **Friends offline while playing:** on by default; turn off if you need party invites (menus may stutter).
* **Smooth mouse fix:** apply/revert the Wine patch.
* **Extra launch options:** passed to TF2 like Steam launch options.

Starting TF2 from the Steam window bypasses the launcher's options. Use tf2mt to play.

## Good to know

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
| `docs/` | measurements and investigations |

## Publishing a release (maintainers)

1. `scripts/package-runtime.sh` → `build/release/tf2mt-runtime-*.tar.xz` (+ `.sha256`) — the runtime with the
   original, unpatched `winemac.so`.
2. `tools/launcher/build.sh --no-install` → `build/launcher/tf2mt-app.zip`.
3. Create a GitHub release and upload both files.
4. Put the runtime's download URL and SHA-256 into `config/release.conf`, commit, and rebuild the app (step 2) so the
   bundled setup downloads the runtime automatically.

See [THIRD_PARTY.md](THIRD_PARTY.md) for the components in the runtime and their licences.
