# M4 report — shader translator v1

Date: 2026-10-07. Gate (PLAN §10): *unit/golden opcode tests green; 100 % of census + VPK corpus translates and
Metal-compiles.* **Result: passed** (details below). Design: [ADR-003](decisions/ADR-003-shader-translator.md).
Semantics rulings: [semantics.md](semantics.md).

## What was built
| Piece | Files |
|---|---|
| Token decoder (SM1.x–3.0 format) | `src/translate/sm.h`, `sm_decode.cpp` |
| MSL emitter + prelude | `src/translate/msl.h`, `msl_emit.cpp`, `msl_prelude.inc` |
| Shader ABI for the backend | `src/translate/msl_abi.h` (bindings, varying slots, vertex-pulling fetch table, driver constants, function constants) |
| CPU reference interpreter | `src/translate/interp.h`, `interp.cpp` |
| Golden opcode tests | `tests/unit/opcodes.mm` (28 cases, GPU and CPU vs hand-computed values) |
| Differential tester | `tools/translate/difftest.mm` (GPU vs CPU, 64 random invocations per shader) |
| Corpus compile/link tool | `tools/translate/mslbatch.mm` (`--scope all`, `--link N`, `--print`, `--dump`) |
| Feature census tool | `tools/translate/smstat.cpp` |
| Build / test | `make translate-tools`, `make test-translate` (translator also builds for x86_64: `build/translate/x86_64.o`) |

## Results
| Test | Result |
|---|---|
| Golden opcode cases (`build/tools/opcodes`) | **28/28** on GPU and CPU |
| Live corpus (ADR-001 scope: 344 vs_2_0, 845 ps_2_x, 1 ps_1_1) translate + Metal compile + specialise | **1,190/1,190** |
| Live corpus GPU vs CPU differential (64 invocations each) | **1,190/1,190** match |
| Render-pipeline link of random live VS×PS pairs (BGRA8 + D32S8) | **500/500** |
| Decoder over the whole corpus (`smstat`) | 319,978 shaders, **0** decode failures |
| Whole corpus translate + Metal compile + specialise (incl. SM3 superset) | **319,978/319,978** (ps_1_1 43, ps_2_0 20,384, ps_2_x 184,279, ps_3_0 104,768, vs_1_1 56, vs_2_0 5,311, vs_3_0 5,137); run with the emitter before the `rcp(−0)` fix, which only adds one prelude helper |
| Whole-corpus differential sample (every 100th VCS shader, 3,188 incl. SM3) | 3,187 match; 1 numeric outlier (below) |

**Mutation check.** The test suite must be able to fail. Emitters with deliberately wrong semantics were run against it:
* lrp operand swap, `rcp` as plain `1/x`, `mova` truncation, `cmp` using `>`, wrong `_bias`, non-legacy multiply, and
  disabled co-issue were each caught by the golden cases (8 of 28 cases failed for the combined mutant, 1 for co-issue);
* the lrp + `rsq` without `abs` mutant made 259 of 1,190 live shaders mismatch in the difftest.

**The one differential outlier** (`corpus/vcs/17ed425dce8a5879.vs.bin`, vs_3_0, 1 of 64 invocations): the output
is `(c − z) · rcp(c' − z)` (fog range). For that random input the denominator was within a few ulp of 0, and the
result ≈ 4·10⁶. A 1-ulp difference in dp4 summation order between GPU and CPU then changes the result by 12 %.
This is catastrophic cancellation in the shader's own math, not a translation difference.

## Measurements
* Mean emitted MSL: ~7.8 kB per live shader (the shared prelude is ~4.4 kB of it).
* Metal front-end compile + specialisation: ~55 ms per shader per thread when idle. The live set (1,190) compiles
  in ~7 s on 10 cores. The full corpus takes 41 min (2,440 s, 10 cores; mean MSL 9.8 kB).
* The census of the full corpus (`smstat`) confirmed the opcode set: pixel shaders use 25 opcodes (counting dcl/def)
  and none, at any version, has flow control. Vertex shaders add `rep`/`if`, `mova` and `sincos`, and `texldl` exists only in vs_3_0, which is unused live. There
  is no predication, `loop`/`call`, `dsx/dsy`, `vPos`/`vFace` or `texldd` anywhere in the 320k shaders.

## Notes and follow-ups
* *Oracle* rulings in `semantics.md` must be confirmed against DXVK pixels once the backend draws (M6/M7): legacy
  multiply, unwritten VS outputs, alpha-test precision, and `texldp` with w ≤ 0.
* Compile cost (55 ms per shader per thread) is the main reason for the M8 cache and pre-warm work. 1,190 shaders
  cold = 7 s of 10-core compile.
* Possible M9 optimisations: function-constant `b#`/`i#`, VS outputs specialised to the bound PS, half precision
  for `_pp`, and turning the legacy multiply off where DXVK parity allows it.
* `tools/replay` (deferred from M3) is still open. Recommended as the first M5 task.
