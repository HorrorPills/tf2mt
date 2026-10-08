# M2 — Wine integration spike (gate G1)

Date 2026-10-07. Mechanism and decisions: [ADR-002](decisions/ADR-002-wine-integration.md).

| G1 criterion | Result |
|---|---|
| Our DLL loads into TF2 without patching Wine | **Pass**: `d3d9.dll` forwarder (game dir) → builtin `tf2mt.dll` → `tf2mt.so` |
| Presents on TF2's own window | **Pass**: `CAMetalLayer` on the game HWND's client view via `macdrv_functions`; screenshot shows tf2mt output |
| ≥ 119.5 fps presented over 60 s | **Pass: 119.65 fps** (12 × 600-frame blocks, 118.4–120.0 each) |
| Input works | **Pass**: owner typed `echo INPUT_OK` blind into the console while tf2mt presented; it reached the game (console.log, ~42 s after the orange screen). `quit` was not observed; TF2 was closed by the script |
| Uninstall restores DXVK | **Pass**: `scripts/layer-uninstall.sh` removes 4 files; game folder verified clean |

Build/run: `make frontend unixlib && scripts/layer-install.sh`, launch TF2 (any way), `scripts/layer-uninstall.sh`.
Diagnostics: `TF2MT_UNIX_LOG=<unix path>` (present-interval stats every 600 frames), `TF2MT_TRACE_DIR`/`TF2MT_TAG`
(frontend log), `TF2MT_VSYNC=0` (no display sync).

Problems found and fixed on the way:
* Wine only resolves a builtin by name if `system32` also has a copy (wineboot normally creates placeholders):
  the install copies `tf2mt.dll` there too.
* No `winebuild` in the runtime: `tools/wine/mark_builtin.py` stamps the "Wine builtin DLL" signature at 0x40.

Next (M3): replace the DXVK caps oracle with tf2mt's own `IDirect3D9` (caps/formats from the census), and the
`tools/replay` harness; confirm the null-renderer frame time matches the Phase-0 ceiling.
