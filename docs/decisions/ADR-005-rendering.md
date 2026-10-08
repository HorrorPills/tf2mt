# ADR-005: Rendering architecture v1 (M6)

Status: accepted (2026-10-07, M6)

## Decision
1. **Command stream instead of per-call unix calls.** The frontend appends compact packets
   (`src/common/commands.h`) for every state change, draw, clear and copy. The backend decodes them in order inside
   one `TF2MT_UNIX_SUBMIT` call.
   * **Flush points:** Present, and before any operation that must not overtake queued work: DISCARD renaming,
     GetRenderTargetData, object destruction, Reset.
   * **Encoder:** the backend decodes on the calling thread for now. PLAN §8.2's separate encoder thread is an
     M9 optimisation.
2. **Submission:** the upload command buffer always commits just before the frame command buffer, under the same
   serial (renamed buffers recycle only after the frame that used them). At most 2 frames are in flight; the wait
   happens in Present, never in draws.
3. **Passes:** one render encoder per run of draws to the same attachments, opened lazily. A full-target Clear
   before any draw becomes the load action; partial clears draw a quad with an internal pipeline.
4. **Pipelines:** the PSO key is (VS, PS, function-constant spec, attachment formats, samples, blend state, write
   mask). Shaders are translated when created and compiled asynchronously (`newLibraryWithSource:completionHandler:`),
   so map loads overlap compiles. Specialisation and PSO creation are synchronous on first use; the miss policy
   is M8.
5. **D3D9 semantics settled by the oracle** (also in docs/semantics.md):
   * half-pixel correction is +0.5 px right/down for Metal;
   * a VS with no PS runs a fixed-function fragment stage that outputs the diffuse colour;
   * Clear covers viewport ∩ scissor ∩ rects;
   * SetRenderTarget(0) resets viewport and scissor;
   * depth bias is scaled by 2^24 (D3D bias is in D24 units; the depth buffer is Depth32Float_Stencil8);
   * winding: front = clockwise.
6. **Constants** go per draw with `set{Vertex,Fragment}Bytes`: 4 KB VS + 3.5 KB PS. A dirty-range constant ring
   is M9.
7. **Validation** is through `tools/replay`: frame dumps via GetRenderTargetData (the identical path on every
   provider) plus `tools/replay/ssim.py`.

## Consequences
* TF2 renders through tf2mt live and in replay with SSIM ≥ 0.9992 against DXVK on menu, MOTD, loadout and world
  scenes (docs/m6-report.md).
* The open items in m6-report.md (queries, LOD bias, mid-frame upload order, static-lock hazards, first-use
  compile stalls) are tracked for M7–M9.
