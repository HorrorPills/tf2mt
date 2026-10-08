// MSL prelude prepended to every translated shader. Mirrors src/translate/msl_abi.h; semantics in docs/semantics.md.
R"MSL(#include <metal_stdlib>
using namespace metal;

// Layouts must match msl_abi.h byte for byte (no float3/uint3: they are 16-byte aligned in MSL).
struct tf2mt_int_consts { int4 i[16]; uint b; uint pad0, pad1, pad2; };
struct tf2mt_fetch { uint stream_type; uint offset; uint stride; uint pad; };
struct tf2mt_vs_driver { float4 half_pixel; float4 clip_plane0; uint4 stream_words; tf2mt_fetch fetch[16]; };
struct tf2mt_ps_driver { float alpha_ref; float pad0, pad1, pad2; float4 lod_bias[4]; };
static_assert(sizeof(tf2mt_int_consts) == 272, "msl_abi.h layout");
static_assert(sizeof(tf2mt_vs_driver) == 304, "msl_abi.h layout");
static_assert(sizeof(tf2mt_ps_driver) == 80, "msl_abi.h layout");

constant int tf2mt_alpha_func_fc [[function_constant(0)]];
constant uint tf2mt_clip_mask_fc [[function_constant(1)]];
constant int tf2mt_alpha_func = is_function_constant_defined(tf2mt_alpha_func_fc) ? tf2mt_alpha_func_fc : 8;
constant uint tf2mt_clip_mask = is_function_constant_defined(tf2mt_clip_mask_fc) ? tf2mt_clip_mask_fc : 0u;

// D3D9 "legacy" multiply: 0 * anything (including INF/NaN) = 0.
static inline float4 lmul(float4 a, float4 b) { return select(a * b, float4(0.0f), a == 0.0f || b == 0.0f); }
static inline float lmul1(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
static inline float4 lmad(float4 a, float4 b, float4 c) { return lmul(a, b) + c; }
static inline float ldot2(float4 a, float4 b) { float4 m = lmul(a, b); return m.x + m.y; }
static inline float ldot3(float4 a, float4 b) { float4 m = lmul(a, b); return m.x + m.y + m.z; }
static inline float ldot4(float4 a, float4 b) { float4 m = lmul(a, b); return m.x + m.y + m.z + m.w; }
static inline float4 fmul(float4 a, float4 b) { return a * b; }
static inline float4 fmad(float4 a, float4 b, float4 c) { return fma(a, b, c); }
static inline float fdot2(float4 a, float4 b) { return dot(a.xy, b.xy); }
static inline float fdot3(float4 a, float4 b) { return dot(a.xyz, b.xyz); }
static inline float fdot4(float4 a, float4 b) { return dot(a, b); }
// pow(x, y) = exp2(y * log2(|x|)) with the legacy multiply, so pow(0, 0) = 1.
static inline float lpow(float x, float y) { return exp2(lmul1(y, log2(abs(x)))); }
// rcp: D3D9 defines rcp(+-0) = +INF (IEEE would give -INF for -0).
static inline float tf2mt_rcp(float x) { return x == 0.0f ? INFINITY : 1.0f / x; }
// nrm of a zero-length vector yields 0 (D3D9 legacy-multiply result of 0 * INF).
static inline float4 tf2mt_nrm(float4 v) { float l = dot(v.xyz, v.xyz); return l > 0.0f ? v * rsqrt(l) : float4(0.0f); }

static inline uint tf2mt_word(device const uint *b, uint words, uint i) { return i < words ? b[i] : 0u; }
// Dynamic vertex pulling. Formats are the census set (FLOAT1-4, D3DCOLOR, UBYTE4, SHORT2); the backend rejects
// declarations using anything else, so the default case is unreachable in practice.
static inline float4 tf2mt_fetch_attr(constant tf2mt_vs_driver &d, uint reg, uint vid,
                                      device const uint *b0, device const uint *b1,
                                      device const uint *b2, device const uint *b3)
{
    tf2mt_fetch e = d.fetch[reg];
    uint s = e.stream_type & 0xffu, type = (e.stream_type >> 8) & 0xffu;
    device const uint *b = s == 0u ? b0 : s == 1u ? b1 : s == 2u ? b2 : b3;
    uint words = d.stream_words[s & 3u];
    uint w = (e.offset + vid * e.stride) >> 2;
    uint x = tf2mt_word(b, words, w);
    switch (type) {
    case 0: return float4(as_type<float>(x), 0.0f, 0.0f, 1.0f);
    case 1: return float4(as_type<float>(x), as_type<float>(tf2mt_word(b, words, w + 1)), 0.0f, 1.0f);
    case 2: return float4(as_type<float>(x), as_type<float>(tf2mt_word(b, words, w + 1)),
                          as_type<float>(tf2mt_word(b, words, w + 2)), 1.0f);
    case 3: return float4(as_type<float>(x), as_type<float>(tf2mt_word(b, words, w + 1)),
                          as_type<float>(tf2mt_word(b, words, w + 2)), as_type<float>(tf2mt_word(b, words, w + 3)));
    case 4: return unpack_unorm4x8_to_float(x).zyxw;                       // D3DCOLOR: bytes B,G,R,A
    case 5: return float4(float(x & 0xffu), float((x >> 8) & 0xffu), float((x >> 16) & 0xffu), float(x >> 24));
    case 6: return float4(float(as_type<short2>(x).x), float(as_type<short2>(x).y), 0.0f, 1.0f);
    default: return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
}

static inline bool tf2mt_alpha_pass(float a, float ref)
{
    switch (tf2mt_alpha_func) {
    case 1: return false;
    case 2: return a < ref;
    case 3: return a == ref;
    case 4: return a <= ref;
    case 5: return a > ref;
    case 6: return a != ref;
    case 7: return a >= ref;
    default: return true;
    }
}
)MSL"
