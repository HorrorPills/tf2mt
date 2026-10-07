# Loadout / class-select stall (2–4 fps) — investigation

Reported by the owner on 2026-10-07: "game drops to 2-3 fps inside the inventory character screen". Their assumption was shader loading.

## Verdict (2026-10-07, confirmed with the owner)
**It is not shaders.** TF2's main-menu friends panel (`CSteamFriendsListPanel`, which stays alive under the loadout and class-select screens) subscribes to `PersonaStateChange_t` and `FriendRichPresenceUpdate_t`. On every friend status or rich-presence change it re-queries all friends (the owner has 208) through the game's `steamclient64.dll`, i.e. **~1,000+ synchronous Steam IPC calls per frame**. Under Wine each call is a pipe round trip through the wineserver (~0.9 ms, against microseconds on Windows), so one UI update takes 0.25–1.5 s.

**Proof (A/B with the owner):** Steam Friends set to Offline → 0 storm seconds in 238 s and no visible stutter. Back Online → stutter started immediately, with 39 storm seconds. Switching offline via `steam://friends/status/offline` → one ~20 s burst (every friend flips to offline), then 0 storms in 125 s.

**Fix applied:** `scripts/tf2.sh` sets Steam Friends offline 5 s *before* starting TF2 (so the flip burst happens before the game listens) and back online when `tf_win64.exe` exits. Opt out with `TF2_FRIENDS_ONLINE=1` (needed for parties and invites). Limitation: games started from the Steam library UI bypass the script.

**Tried and failed:** a HUD override replacing `SteamFriendsList` in `resource/ui/mainmenuoverride.res`. The panel is created in code, so the `.res` only changes layout. There is no cvar or launch option for the panel, and `-gamepadui` isn't shipped for TF2. Patching the game is excluded (VAC). Steam's `log_ipc` doesn't see the game-side calls.

**Remaining structural option:** faster game↔Steam IPC under Wine. During storms the wineserver is the bottleneck (70–84% CPU, x86 under Rosetta, ~0.6 ms of server CPU per round trip). A native arm64 wineserver would help but probably not enough (~0.3 ms × 1,000 calls is still a stall).

## Reproduction
`tools/bench/repro-loadout.sh <tag> <dxvk|null>` starts a local `koth_harvest_final` (`sv_lan 1`), opens the loadout with `open_charinfo_direct` over RCON and records frames, threads, process CPU and GPU utilisation. Options: `VPROF=1` (Source VProf spike dumps), `CONTS=1` (timestamped console), `LOADOUT_S`, `STALL_SAMPLE=1`. The stall is **intermittent**: roughly 1 run in 3. It comes as episodes of 1 s to 2 min with frames of 250–1,500 ms. The class-select screen is affected too.

## Evidence chain
| Hypothesis | Test | Result |
|---|---|---|
| Shader/pipeline compile | cold run (DXVK state cache off, Metal shader cache moved away); per-thread CPU of `dxvk-shader`/`dxvk-pcompiler`; `MTLCompilerService` CPU | **No**: cold run clean; compiler threads and Metal compiler at 0% during stalls |
| GPU-bound | `ioreg` device utilisation during a 2-minute stall | **No**: GPU 0% |
| macOS timer throttling / App Nap | in-process `Sleep(1)` probe | **No**: 1.25 ms throughout |
| Focus/limiter cvars | RCON: `engine_no_focus_sleep 0`, `host_sleep 0`, `fps_max 400` | **No** |
| Memory/paging | `vm.swapusage`, memory pressure | **No**: no swap, 81% free |
| Audio (CoreAudio, USB interface) | Wine fixme timeline | **No**: only at startup |
| Cocoa main thread / cursor | `sample` of the Cocoa thread during stall | **No**: idle in event loop |
| SteamNetworkingSockets lock (ping probes) | timestamped console | **No**: only ~10 ms holds during stalls |
| DNS / network config (`iphlpapi`, `nsi`) | Wine traces | **No**: a few hundred calls per session |
| msync vs non-msync | runs in both modes | **Not the fix**: stalls occur in both (an early "msync fixes it" reading came from too few runs) |
| **Where the frame goes** | **Source VProf `vprof_dump_spikes 200`** | every stalled frame (1.3–1.5 s) is **~99% `CEngineVGui::Simulate`** (UI think), with no profiled children; Paint < 1 ms |
| **What the main thread does** | Wine `+file` trace aligned with frame times | in each stall second the main thread does **~8,000 file ops vs ~700 normally**: thousands of tiny `NtWriteFile`/`NtReadFile` (5/4-byte headers + 12–26-byte payloads) on an **anonymous pipe pair** created at startup |
| **Who answers** | per-process CPU at 1 Hz | every stall second: **`steam.exe` 1% → 10–13%, wineserver 20% → 70–84%, game 200% → 20–50%** |

So the stall is a synchronous request/response storm between the game's `steamclient64.dll` and `steam.exe` over Wine pipes, triggered from inside TF2's UI update. The intermittency apparently depends on Steam-client or UI state; it did not occur in a fresh Steam session started with `-console`.

## Side findings
* Wine logs `err:font:alloc_font_handle out of realized font handles` 832× during map load. It isn't the stall cause, but it can affect text rendering.
* The game writes an `assert_tf_win64.exe_*.dmp` minidump once per session at map load (a ~2 s freeze at load).
* `sample` (macOS) is very intrusive under Rosetta: 8 s samples *created* stalls in null-renderer runs. Don't profile with it during measurements.
