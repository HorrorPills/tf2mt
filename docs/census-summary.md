# M1 census — scope summary for the tf2mt renderer

Source data: [census-report.md](census-report.md), generated from 4 census sessions (2026-10-07). Each session ran
8 maps with 22 bots and first-person spectating, plus the loadout screen and screenshots: base settings, MSAA 4×,
`mat_hdr_level 2`, `mat_queue_mode 0`. Shader corpus: 1,190 shaders captured live, plus 318,788 extracted from the
game's `.vcs` containers ([tools/census/vcs.py](../tools/census/vcs.py)). 1,014 of 1,015 live shaders also appear in
the containers. The one exception is a 44-byte `ps_1_1` the engine builds itself.

**Settings caveat:** these results reflect the owner's mastercomfig `low` preset and the adapter identity DXVK reports.
Higher presets (shadows, flashlight, phong) and dxlevel 98 (SM3) exercise more features. See "Not yet covered".

## What TF2 uses (and therefore what tf2mt implements)

| Area | Used | Not used (stub, fail loud in debug) |
|---|---|---|
| Device | `Direct3DCreate9Ex` + plain `CreateDevice`, flags `FPU_PRESERVE|MULTITHREADED|PUREDEVICE|HWVP`; windowed, 1 back buffer, D24S8 auto-depth, `INTERVAL_IMMEDIATE` | `CreateDeviceEx`, state blocks, FVF, fixed-function lighting/transform |
| Draws | `DrawIndexedPrimitive` only: TRIANGLELIST (99.5 %), TRIANGLESTRIP; INDEX16; base vertex 0; ≤ ~1,000 draws/frame | `DrawPrimitive`, `*UP` draws, instancing (`SetStreamSourceFreq`), 32-bit indices |
| Vertex input | Declarations with FLOAT1–4, D3DCOLOR, UBYTE4 (normals/tangents), SHORT2 (blend weights); up to 3 streams; **stride-0 streams** (stream 2 constant attribute); strides 4–96 | FVF, other decl types |
| Buffers | Static VB/IB (WRITEONLY, lock 0); **dynamic VB/IB: `NOOVERWRITE` ×10.6 M vs `DISCARD` ×76 K** (≈1/frame) → append ring | READONLY buffer locks |
| Textures | DXT1, DXT5 (dominant), A8R8G8B8, X8R8G8B8, L8, A16B16G16R16 (HDR 2), Q8W8V8U8 / A16B16G16R16F uploads; cube and volume (32³ colour-correction LUT); **always uploaded via SYSTEMMEM staging + `UpdateTexture`/`UpdateSurface`**; dynamic A8R8G8B8 locked directly | Direct LockRect on DEFAULT textures, auto-gen mips, palettes |
| Render targets | RT0 only (A8R8G8B8, X8R8G8B8 targets); depth D24S8; MSAA 4× back buffer (variant) | MRT 1–3 (always NULL), float RTs, depth textures (INTZ/DF24) at this preset |
| Copies | `StretchRect` RT→RT **scaled + linear**, sub-rects; **MSAA source → non-MSAA, scaled** (resolve+scale); `GetRenderTargetData` (screenshots) | `ColorFill` |
| Clears | Z|STENCIL full; Z with rect | colour clears via `Clear` |
| Viewport | depth range 0–1 and **0–0.1** (viewmodel) | — |
| Clipping | user clip plane 0 (water) | scissor |
| Queries | **OCCLUSION ×1.19 M** (polled without flush, ~10 % not ready); **EVENT** once per frame with FLUSH (frame-latency limiter) | timestamp queries |
| Samplers | WRAP/CLAMP; point/linear/aniso (×4); mip none/point/linear; LOD bias 0–3; MAXMIPLEVEL; **SRGBTEXTURE per sampler**; vertex textures unused | border colours other than 0, mirror |
| States | 103 render states set (incl. sRGB write, depth bias, stencil, alpha test, colour write masks); texture-stage states are vestigial (50 sets total) | — |

## Shaders: SM2 path only (see ADR-001)

* Live: `vs_2_0` ×344, `ps_2_x` (`ps_2_1` token) ×845, plus 1 engine `ps_1_1`.
* **Pixel shaders, 23 opcodes, no flow control:** mov tex mul mad add rcp cmp min dp3 lrp nrm rsq max exp log pow frc dp4
  dp2add abs **texldp** **texkill** nop. Samplers: 2D (2,035 decls), cube (74), volume (50). One shader writes depth (`oDepth`).
* **Vertex shaders, 26 opcodes:** mov mad dp4 mul add max dp3 nrm **mova** rcp rsq slt min abs pow log exp **rep/endrep**
  (80, static loop count from `i#`) **if/else/endif** (8, `b#`) lrp frc sge sincos. **Relative addressing ×2,551** (skinning).
  Float constants up to c6x statically indexed, more via `a0`.
* Modifiers: source `neg` (ps also `_abs` via ABS); destination `_sat`, `_pp`, `centroid` (vs outputs).
* Shipped superset: `ps_3_0` ×104,768 and `vs_3_0` ×5,137 exist (dxlevel 98) but **are never selected** with the current
  adapter identity.

## Implications for the design (PLAN.md §8)

1. **Translator (M4):** the SM2 subset above, a much smaller surface than the full SM2/3 spec the plan budgeted.
   PS has no control flow. VS needs `rep`/`if` with static constants (these specialise well as function constants),
   `mova` + relative constant indexing, and `sincos`.
2. **Vertex pulling:** must support stride-0 streams and the UBYTE4/SHORT2/D3DCOLOR decodes. Three streams cover everything.
3. **Dynamic ring:** one `DISCARD` per frame, then NOOVERWRITE appends: exactly the ring design in §8.1.
4. **Uploads:** staging (SYSTEMMEM) textures can stay CPU-side, with `UpdateTexture` as the single upload path.
   DEFAULT-pool textures are never locked except dynamic A8R8G8B8.
5. **Render passes:** single colour attachment + D24S8 (→ Metal `Depth32Float_Stencil8`). Resolve-and-scale
   `StretchRect` needs a small internal blit/resolve pass.
6. **Queries:** occlusion results must arrive within a frame or two (≈10 % are polled before ready today).
   The EVENT+FLUSH per frame is Source's frame-latency limiter; the backend can satisfy it from command-buffer completion.

## Not yet covered (next census passes when relevant)

Higher graphics presets (shadows → depth textures, flashlight → projected textures, phong/rim lighting), dxlevel 98
(SM3), mat_dxlevel 90, MvM, resolution changes / `Reset`, alt-tab. These need the owner's settings changed temporarily
and are scheduled before M7 (full feature coverage). The DoD for M1 ("census complete; ≥ 8 scenario traces; corpus
extracted") is met for the owner's configuration.
