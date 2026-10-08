# ADR-004: Resource model and the capture/replay harness (M5)

Status: accepted (2026-10-07, M5)

## Context
PLAN §8.1 prescribes the locking model and §8.3 the format mapping. M5's gate: replay of three scenarios creates every
resource without error, with upload bandwidth within ledger targets. M3 had deferred `tools/replay`. The census
(docs/census-summary.md) narrows the problem:
* uploads come through SYSTEMMEM staging plus `UpdateTexture`/`UpdateSurface`;
* the only directly locked DEFAULT textures are dynamic A8R8G8B8;
* dynamic VB/IB use about one DISCARD per frame, followed by NOOVERWRITE appends.

## Decision
1. **Capture format `.t9`** (`tools/trace/t9.h`). The trace proxy's capture mode (`tools/trace/capture.c`) records
   every device and resource call TF2 makes:
   * fixed binary records, objects referenced by 32-bit ids;
   * lock contents stored as tightly packed rows at Unlock;
   * one recursive lock, so records keep the true call order;
   * calls made inside the oracle and from oracle-owned threads are filtered out.
   An audit thunk logs any device method without a recorder. TF2 calls none.
2. **Replay lifetimes follow the capture, not the provider's refcounts.** DXVK's public refcounts include internal
   references that the game never sees, so mirroring Release counts freed live objects early. The replayer
   (`tools/replay/replay.c`) holds one reference of its own per mapped object and drops it when the captured count
   reaches 0.
3. **Buffers** are `MTLStorageModeShared`. The PE frontend writes straight into `contents`: same process, unified
   memory, and Lock is pointer arithmetic with no unix call.
   * **Dynamic buffers** are renamed on DISCARD. The old backing is retired with the next submission serial and
     recycled once the GPU completes it.
   * **Present** always advances the serial, committing an empty command buffer if nothing is pending.
4. **Textures** are `MTLStorageModePrivate`, so Apple GPUs can use lossless compression.
   * **Uploads** are packed into a write-combined shared staging ring (32 MB chunks, recycled on completion) and
     copied with blits in submission order. Pending blits are committed at Present or when a chunk fills.
   * **SYSTEMMEM/SCRATCH** resources stay CPU-only.
   * **DYNAMIC and MANAGED** textures upload the locked region at Unlock.
   * **UpdateTexture** uploads every destination level from the matching source level, without dirty-rect
     tracking. Census traffic doesn't justify the complexity yet.
5. **Format mapping**: every format the adapter claims has a Metal mapping (`src/unixlib/resources.mm`). Sampling
   swizzles (X8, L8, A8L8, V8U8, R32F) are recorded per format and applied through texture views when bound (M6).
   The 16-bit packed formats and X8L8V8U8 are claimed by the census tables but never used. Their channel order is
   marked unverified until a pixel test (M7).
6. **Verification**: `TF2MT_VERIFY_UPLOADS=1` reads every upload back and compares it byte for byte with its source.

## Consequences
* tf2mt replays TF2 captures with no failed calls and byte-exact uploads (docs/m5-report.md).
* Uploads cost CPU memcpy (staging) plus one GPU blit. Map load moved 3.5 GB in 0.24 s of upload calls.
* Hazards on renamed buffers are covered. Hazards on textures updated mid-frame are covered by command-buffer order
  once M6 encodes draws on the same queue after the upload blits.
* GetRenderTargetData reads back synchronously; it's used only for screenshots.
