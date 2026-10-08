# Wine/Steam processes that never closed (fixed in v0.3.1)

**Report (Reddit):** "It runs well but the wine processes never close eating up ram and cpu until you need to restart the Mac." **Confirmed.**

## Causes
1. **Steam was never shut down.** It runs silently with no window. After TF2 or the launcher closed, Steam, the wineserver and Steam's web UI kept running: measured at 70–100 % CPU and ~900 MB in the web UI, plus 50 % CPU in the wineserver. The Friends-restore step even started Steam *again* after TF2 exited.
2. **Orphans after a dead wineserver.** When a wineserver dies abruptly, its clients (`winedevice.exe` etc.) stay alive with ppid 1, each burning ~2 % CPU. Found: 6 of them, 9–29 h old. They also stopped the next Wine session for the same prefix from starting until they were removed. That fits the "restart the Mac" part of the report.
3. **macOS 27 "running in background".** macOS attributes processes to the app that started them. After the launcher quit, the Dock kept tf2mt with a dot and "Stop Running in Background", and choosing it closed Steam under a running game. Starting the processes in their own session (`POSIX_SPAWN_SETSID`) or with responsibility disclaimed did not change this, and neither did a `launchctl submit` job. A helper app opened via LaunchServices did (tested with a throwaway app).

## Design (no installs, no admin rights, nothing outside tf2mt.app)
* **`tf2mt.app/Contents/Helpers/tf2mt Session.app`** (`tools/launcher/session/TF2Session.swift`, `LSUIElement`). The launcher opens it with `open -n -g -j … --args <script>` to run every script that can start Wine: `play.sh`, `steam.sh`, `install-tf2.sh`, `setup.sh`, `session-helper.sh`. Output and exit code are returned through `$TF2_HOME/run/req-*.log(.status)`. The Session app exits after its script, so Steam/TF2 belong to no visible app and quitting tf2mt really quits it.
* **`scripts/session-helper.sh`** runs one per session (pid in `$TF2_HOME/run/helper.pid`) and lives exactly as long as the session. It ends the session when TF2 has run and exited (3 s), or when no TF2 runs, no launch is pending and the launcher has been gone for 10 s. If a new launch starts during teardown, it keeps watching that launch.
* **`scripts/session-end.sh`**: restores Friends (only if `tf2.sh` set them offline), runs `steam -shutdown` (≤ 30 s), then `wineserver -k` if needed, then the orphan sweep, then removes the renderer layer. Idempotent, with a lock (`run/teardown.lock`).
* **`scripts/wine-sweep.sh`** kills only processes that map *this* runtime's `ntdll.so` (other Wine installs are never touched), and only when their wineserver is gone or younger than they are. It runs before Steam starts (`steam.sh`), when the launcher starts (the helper sweeps if there's nothing to manage), and at teardown.
* **`play.sh`** writes `run/launch-pending` and waits for a running teardown. The **launcher** writes `run/launcher.pid`, starts the helper on start and before every Steam/TF2 action, and on quit (Dock, ⌘Q, closing the window) hands over and quits at once.

## Owner-run tests (2026-10-08, `tools/bench/session-watch.sh`)
| Quit path | Result |
|---|---|
| Quit TF2 (Dock), launcher stays open | everything gone 14 s after TF2 |
| Quit TF2, then the launcher within 2 s | clean in 20 s |
| Quit the launcher first while in game, then TF2 | launcher fully quits (Dock: not running), TF2 unaffected, clean 12 s after TF2 |
| Force-quit the launcher in game, then TF2 (Dock) | TF2 unaffected, clean in 11 s |
| Start Steam only, quit the launcher | 10 s grace, then clean `steam -shutdown` |
| Quit TF2 and press Play within ~3 s | Steam reused (no teardown), new game fine, clean afterwards |

Also tested: an orphan sweep in a test prefix (8 orphans of a killed wineserver, all removed, a new session starts afterwards), and the updater through LaunchServices (the shipped v0.3.0 and the new build both swap and relaunch).
