# ADR-003: Shader translator architecture (M4)

Status: accepted (2026-10-07, M4)

## Context
PLAN.md §8.4 sketches parse → "SSA-ish" IR → specialisation → MSL emitter, plus a CPU reference interpreter for
semantics tests. The M1 census (ADR-001) shrank the problem: TF2 runs `vs_2_0` + `ps_2_x` (plus one engine `ps_1_1`).
These use 23 PS and 26 VS opcodes (census), with structured static flow control in the VS only (`rep i#`, `if b#`). A decode of the
whole corpus with `tools/translate/smstat.cpp` (319,978 shaders, 0 decode failures) confirms the shipped SM3 superset
adds only `texldl`, input/output declarations and more registers. It has no predication, `loop`/`call`, `dsx/dsy`,
`vPos`/`vFace`, `texldd` or `breakc`.

## Decision
1. **No separate IR.** The decoder (`src/translate/sm.h`, `sm_decode.cpp`) produces a flat instruction list. The
   emitter maps each D3D register to an MSL `float4` local and each instruction to one masked assignment. The Metal
   compiler (LLVM) already does SSA construction, dead-lane elimination, constant folding and register allocation,
   so a hand-written SSA IR would add a second optimiser without a measured benefit. Revisit if M9 profiling shows
   MSL compile time or shader quality issues that an IR would fix. AIR emission, a later option under D5, would also
   need one.
2. **Scope = census (D9).** The emitter accepts vs_1_1/2_0/2_x/3_0 and ps_1_1–1_3/2_0/2_x/3_0 bytecode, but only the
   opcodes, modifiers and register types seen in the corpus. Anything else fails with the shader version, token
   offset, opcode and the unsupported feature. Translating the SM3 superset as well is nearly free and keeps
   ADR-001's later dxlevel-98 option open. The M4 gate stays the ADR-001 live set.
3. **One translation per shader, specialised by function constants**: alpha-test function (fc 0) and clip-plane
   mask (fc 1). `b#`/`i#` stay buffer reads for now. They are uniform branches, and turning them into function
   constants is an M8/M9 measurement. Shadow-compare samplers are not specialised, because the census has no depth
   textures at this preset. Add an fc when a census pass shows them.
4. **ABI** (`src/translate/msl_abi.h`, mirrored in `msl_prelude.inc`):
   * float constants in `buffer(0)`, `i#`/`b#` in `buffer(1)`, driver constants in `buffer(2)`, vertex streams in
     `buffer(3..6)`, `s#` → `texture(#)`/`sampler(#)`;
   * fixed varying slots (pos, c0, c1, t0–t7), so any VS links with any PS without per-pair recompiles;
   * dynamic vertex pulling, so vertex declarations never multiply pipelines (D5).
   Reflection (`msl::Reflection`) tells the backend which bindings, inputs and samplers a shader uses.
5. **Two-sided verification.** `src/translate/interp.cpp` is an independent CPU interpreter written from the D3D9
   definitions and `docs/semantics.md`. It shares only the decoder with the emitter. The emitter also has a
   `Mode::ComputeTest` that wraps the same body in a kernel with buffer I/O, explicit-LOD sampling and a recorded
   texkill:
   * `tests/unit/opcodes.mm`: golden opcode cases with hand-computed values, checked on GPU and CPU;
   * `tools/translate/difftest.mm`: every corpus shader on GPU vs CPU with random inputs;
   * `tools/translate/mslbatch.mm`: Metal compile and specialisation of the corpus in production mode.
   Mutation runs (deliberately broken emitters) confirmed that the golden tests and the difftest detect wrong
   semantics.
6. **Compile mode:** `MTLMathModeRelaxed` (INF/NaN preserved), MSL 3.1.

## Consequences
* The translator is ~1,200 lines of C++20 (decoder, emitter, ABI, prelude) and builds for x86_64, so it can link into `tf2mt.so` (D2) unchanged.
* The emitted MSL is verbose (mean ~7.7 kB per live shader) and the Metal front-end compile costs ~55 ms per shader
  per thread. The persistent cache and pre-warm (M8) must hide this. 1,190 live shaders take ~7 s on 10 cores.
* Every VS writes 10 float4 varyings, even if the PS reads fewer. Specialising VS outputs by the bound PS is a
  possible M9 optimisation (function-constant-guarded outputs).
