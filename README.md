# tf2mt — Team Fortress 2 tuned for Apple Silicon

<img width="1092" height="834" alt="Screenshot 2026-10-08 at 13 12 27" src="https://github.com/user-attachments/assets/a286a706-6c99-44f1-b56a-120eae0c0b9c" />

Play the Windows version of **Team Fortress 2** on Apple Silicon Macs, smoothly and with low input lag.
tf2mt is a simple Mac launcher with its own Direct3D 9 → Metal renderer and a set of tested fixes for Wine.
Works on VAC-secured servers.

## Requirements

- Apple Silicon Mac, macOS 13 or newer
- About 35 GB of free disk space
- A Steam account (TF2 is free)

## Install

1. Download **tf2mt-app.zip** from [Releases](../../releases), unzip it and move **tf2mt.app** to Applications.
   On first launch: right-click → **Open**.
2. In tf2mt, open the **Setup** tab and click **Run setup**.
3. Click **Log in to Steam**, then **Install TF2**.
4. Click **Play**.

The launcher updates itself when a new version is out.

## What it fixes

- **Smooth aim:** mouse movement reaches the game every frame, with less input lag.
- **No menu stutter:** loadout and class select stay smooth.
- **Fewer hitches and no tearing.**
- **Clean exit:** Steam and Wine close when you quit TF2.

## Good to know

- tf2mt doesn't modify TF2 or its memory. It's a Wine setup, like Proton on Steam Deck.
- Nothing from Valve is redistributed: Steam and TF2 download from Valve.
- Start TF2 from tf2mt, not from the Steam window, or the launcher's settings won't apply.
- Logs: `~/Games/tf2/logs` (**Logs** button).

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

## More

- [Current status](docs/STATUS.md) · [Renderer plan](PLAN.md)
- [Mouse input](docs/mouse-input.md) · [Menu stutter](docs/loadout-stall.md) · [Benchmarks](docs/phase0-report.md) · [Session cleanup](docs/session-lifetime.md)
- [Third-party licences](THIRD_PARTY.md)
- Graphics presets by [mastercomfig](https://github.com/mastercomfig/mastercomfig)
