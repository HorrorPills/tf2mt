# ADR-002: Wine integration — tf2mt builtin pair + d3d9 forwarder; CAMetalLayer via this Wine build's macdrv table

Status: accepted (2026-10-07, M2 / gate G1). Supersedes PLAN.md D3 (replacing Wine's builtin d3d9) and settles R1/R2.

## Decision
1. **Deployment (R2):** tf2mt ships as a *new* builtin pair `tf2mt.dll` (x86_64 PE, stamped with Wine's builtin
   signature at offset 0x40) + `tf2mt.so` (x86_64 Mach-O exporting `__wine_unix_call_funcs`), installed into the
   runtime's `lib/wine/x86_64-{windows,unix}`. A copy of `tf2mt.dll` also goes in the prefix's `system32`, because Wine
   resolves builtins by name through system32, like the placeholders wineboot creates. TF2 loads a tiny `d3d9.dll`
   **forwarder beside tf_win64.exe** whose exports forward to `tf2mt.*`. Wine's own `d3d9.dll` and the prefix's DXVK
   are **not modified**. `scripts/layer-uninstall.sh` removes four files and TF2 is back on DXVK.
2. **PE ↔ unix calls:** Wine's public builtin design: `NtQueryVirtualMemory(..., MemoryWineUnixFuncs=1000)` →
   handle → `ntdll.__wine_unix_call_dispatcher(handle, code, args)` → `__wine_unix_call_funcs[code](args)`.
   Declared in our own code (`src/frontend/device.c`); no Wine source copied. Parameter structs are fixed-width
   (`src/common/unix_calls.h`).
3. **Window → Metal (R1):** this Wine build (Sikarugir 10.0) exports a data symbol `macdrv_functions`, a table of 10
   function pointers, which upstream Wine 10 does not have. It exists as the interface DXMT's `winemetal.so` uses.
   tf2mt uses entries 1, 2, 6, 7, 8: `get_win_data(hwnd)` → `client_cocoa_view` (4th field of the record) →
   `view_create_metal_view(view, MTLDevice)` → `view_get_metal_layer(metal_view)` → `CAMetalLayer`. The table layout
   was taken from DXMT's public source (MIT) **for interface compatibility only**; no DXMT code is used. Wine keeps
   ownership of the window, view, focus and input.

## Evidence (M2 spike, 2026-10-07)
* `tf2mt.dll` loaded as builtin; unix lib handle obtained; `macdrv_functions` found; layer attached to TF2's HWND.
* Null-renderer device + clear-to-colour Present, vsync on: **119.65 fps over 60 s** (12 × 600-frame blocks, each
  118.4–120.0 fps; per-block present-interval stdev 0.56–1.98 ms, spikes are the game's own menu work).
* Screenshot: TF2's window shows the tf2mt-presented colour.

## Consequences / risks
* Tied to this runtime's `macdrv_functions` ABI (same dependency as DXMT). On an unknown Wine build, `tf2mt.so` init
  fails cleanly (`macdrv_functions` missing); a fallback via the ObjC runtime (find `WineContentView`, add our own
  `CAMetalLayer` sublayer) remains possible (PLAN D4 secondary).
* The tf2mt runtime release must carry `tf2mt.dll/.so` (or setup installs them) once the renderer ships.
