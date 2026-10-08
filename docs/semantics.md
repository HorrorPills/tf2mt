# D3D9 shader semantics in tf2mt (rulings)

PLAN.md D8 asks for a written ruling wherever D3D9 behaviour is ambiguous or differs from IEEE/Metal defaults.
Each row is implemented twice, in the MSL emitter (`src/translate/msl_emit.cpp`, `msl_prelude.inc`) and in the CPU
reference interpreter (`src/translate/interp.cpp`). Each row also has a golden case in `tests/unit/opcodes.mm`.
Rows marked *oracle* still need confirming against DXVK pixels at M6/M7. If DXVK differs, the ruling is revisited here.

## Arithmetic
| Topic | Ruling | Golden case |
|---|---|---|
| Multiply | **Legacy multiply**: `0 * x = 0` for any x, including INF and NaN, in `mul`, `mad`, `dp2add`, `dp3`, `dp4` and inside `pow`. Implemented as `select(a*b, 0, a==0 \|\| b==0)`. `msl::Options::legacy_mul` can turn it off (M9 perf experiment). *oracle* | mul, mad, dp3/dp4 |
| `mad` | `lmul(a,b) + c`, never fused across instructions | mad |
| `rcp` | `x == ±0 → +INF` (D3D9 definition: IEEE would give −INF for −0), else `1/x` | rcp |
| `rsq` | `rsqrt(abs(x))`; `rsq(0) = +INF` | rsq |
| `log` / `exp` | `log2(abs(x))`, `log(0) = −INF`; `exp2(x)` (full precision, not `expp`) | log/exp |
| `pow` | `exp2(y * log2(abs(x)))` with the legacy multiply, so `pow(0,0) = 1` and `pow(−2,2) = 4` | pow |
| `nrm` | `v * rsqrt(dot3(v,v))` on all four components; zero-length vector → 0 | nrm |
| `frc` | `x − floor(x)`, clamped below 1 (`fract`) | frc |
| `cmp` | `src0 >= 0 ? src1 : src2` per component; `−0 >= 0` is true | cmp |
| `cnd` (ps_1_x) | `src0 > 0.5 ? src1 : src2` | ps_1_1 cnd |
| `lrp` | `src2 + src0 * (src1 − src2)` | lrp |
| `dp2add` | `a.x*b.x + a.y*b.y + c.<first swizzle component>` | dp2add |
| `sincos` (vs_2_0, 3 operands) | `x = cos(s)`, `y = sin(s)` through the write mask; operands 2 and 3 (Taylor constants) are ignored | sincos |
| `slt` / `sge` | 1.0 / 0.0 per component | slt/sge |
| `min` / `max` | Metal `min`/`max` (non-NaN operand wins) *oracle* | min/max |
| Scalar operands | `rcp rsq exp log pow sincos` read the component in the last swizzle slot (the assembler always emits a replicate swizzle) | rcp… |

## Modifiers and registers
| Topic | Ruling | Golden case |
|---|---|---|
| `_sat` | `clamp(x, 0, 1)`; NaN → 0 | _sat |
| `_pp` | ignored: everything is 32-bit float (half precision is an M9 experiment) | — |
| Source modifiers | neg, abs, absneg; ps_1_x: bias (x−0.5), bx2 (2x−1), comp (1−x), x2, and their negations | modifiers, ps_1_1 |
| ps_1_x result shift | multiply by 2^shift before `_sat` | ps_1_1 shift |
| ps_1_x co-issue | both instructions of a pair read their sources before either result is written | ps_1_1 co-issue |
| ps_1_x constants | `c#` and `def` values clamped to [−1, 1] when read | ps_1_1 clamp |
| ps_1_x output | `r0` is the colour output | ps_1_1 |
| Temporaries | initialised to 0 (D3D9: undefined) | — |
| `def` | overrides the API constant at that index, also for relative reads | mova/relative |
| `mova` (vs_2_0) | `a0 = floor(x + 0.5)` (round to nearest, halves up) | mova |
| Relative constant read | `c[a0.<c> + n]`: index outside [0, 256) reads 0 | mova/relative |
| `rep i#` | iteration count = `clamp(i#.x, 0, 255)` | rep |
| `if b#` | bit # of the boolean constant mask; `defb` overrides | if/else |

## Textures
| Topic | Ruling | Golden case |
|---|---|---|
| `texld` | sampler dimension from `dcl_2d/cube/volume`; coordinate `.xy` / `.xyz` | texld |
| `texldp` | coordinate divided by `.w` (the w ≤ 0 behaviour is *oracle*, PLAN §9 item 4) | texldp |
| `texldb` | `bias(coord.w)` | — (not in corpus) |
| Sampler swizzle (ps_2_x) | applied to the sampled value | texld swizzle |
| `texkill` | discard when any write-mask component of the operand is < 0; ps_1_x tests `.xyz` of the texture coordinate | texkill |
| ps_1_1 `tex t#` | samples `s#` (2D) at texture coordinate set #; `texcoord t#` = `saturate(float4(tc#.xyz, 1))` | ps_1_1 tex |
| Missing texture channels / sRGB | not a shader concern: Metal texture-view swizzle / sRGB view in the backend | — |

## Stage interface
| Topic | Ruling |
|---|---|
| Varyings | Fixed slots shared by all shaders: pos, c0, c1, t0–t7 (`msl_abi.h`). Every VS writes all slots; each PS declares only what it reads, matched by `[[user(name)]]`. |
| Unwritten VS outputs | `(0, 0, 0, 1)` *oracle* |
| VS colour outputs (SM < 3) | saturated to [0, 1] (D3D9 colour interpolators) |
| `oFog` | dropped: the census never enables fixed-function fog (FOGENABLE = 0 in all 50 sets) |
| `oPts` | `[[point_size]]` only when written (the census draws no points) |
| Half-pixel offset | VS epilogue: `pos.xy += (+1/vp.w, −1/vp.h) * pos.w` (driver constant): D3D9 pixel centres are at integer coordinates, Metal's at +0.5, so geometry moves +0.5 px right/down. PLAN §8.4 wrote the opposite sign; the M6 pixel comparison measured a 1-px up-left shift with it (texel-aligned UI snaps a half-pixel error to a whole pixel) and an exact match after the fix. |
| User clip plane 0 | `[[clip_distance]]` = `dot(pos, plane)` in clip space, before the half-pixel shift, when function constant `clip_mask` bit 0 is set (the census uses plane 0 only, for water) |
| Alpha test | PS epilogue on `oC0.a`, D3DCMPFUNC from function constant 0, reference from the driver buffer (`ALPHAREF/255`) *oracle* (D3D9 compares 8-bit values) |
| Centroid | `dcl_centroid` → `centroid_perspective` on that PS input |
| Vertex input | Dynamic vertex pulling (`tf2mt_fetch_attr`), census formats only (FLOAT1–4, D3DCOLOR as BGRA → RGBA, UBYTE4, SHORT2); missing element → (0,0,0,1); reads are bounds-clamped to the bound buffer size |

## Metal compilation
`MTLMathModeRelaxed`: fast math that keeps INF/NaN semantics. The default `fastMathEnabled` mode lets the compiler
assume no INF/NaN, which would break `rcp(0)`, the legacy multiply and `_sat(NaN)`. Transcendentals still use
Metal's fast variants: differential tests allow 2e-3 relative/absolute error.
