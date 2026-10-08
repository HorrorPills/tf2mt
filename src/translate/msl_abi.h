/* tf2mt shader ABI — the contract between translated MSL (src/translate/msl_emit.cpp) and the Metal backend.
 * Plain C, included by the backend (C++/ObjC++) and mirrored verbatim in the MSL prelude (msl_prelude.inc).
 * Every member is 4-byte scalar or 16-byte vector-shaped so C and MSL layouts are identical (asserted below).
 * Census references (PLAN D9): docs/census-summary.md "Vertex input", "Clipping", "States", "Samplers".
 *
 * Bindings (vertex and fragment stages each have their own tables):
 *   buffer(0)  float constants: VS 256 x float4, PS 224 x float4 (SetVertex/PixelShaderConstantF)
 *   buffer(1)  struct tf2mt_int_consts (i# and b#), only when reflection.uses_int_consts
 *   buffer(2)  struct tf2mt_vs_driver / tf2mt_ps_driver
 *   buffer(3..6) VS: vertex streams 0..3 as raw 32-bit words (dynamic vertex pulling, PLAN D5)
 *   texture(n)/sampler(n)  D3D sampler register s# (PS: s0-s15, VS: s0-s3 = D3DVERTEXTEXTURESAMPLER0-3)
 *
 * Function constants (specialisation, PLAN D5):
 *   0  int  alpha_func  D3DCMPFUNC (1 NEVER .. 8 ALWAYS); default 8. PS epilogue compares oC0.a with ps_driver.alpha_ref.
 *   1  uint clip_mask   bit 0 = user clip plane 0 enabled (census: only plane 0 is used); default 0. VS epilogue.
 */
#ifndef TF2MT_MSL_ABI_H
#define TF2MT_MSL_ABI_H
#include <stdint.h>

#define TF2MT_VS_FLOAT_CONSTS 256
#define TF2MT_PS_FLOAT_CONSTS 224
#define TF2MT_MAX_STREAMS 4
#define TF2MT_MAX_VS_INPUTS 16

#define TF2MT_FC_ALPHA_FUNC 0
#define TF2MT_FC_CLIP_MASK 1

/* Varying slots shared by every VS output struct and PS input struct; also the slot numbering of the compute test
 * harness (see Mode::ComputeTest). */
enum tf2mt_varying {
    TF2MT_VAR_POS = 0, TF2MT_VAR_C0 = 1, TF2MT_VAR_C1 = 2, TF2MT_VAR_T0 = 3, /* .. T7 = 10 */
    TF2MT_VAR_FOG = 11, TF2MT_VAR_PSIZE = 12, TF2MT_VAR_COUNT = 13
};
/* Compute-test PS output slots */
enum tf2mt_ps_test_out { TF2MT_PSO_C0 = 0, /* .. C3 = 3 */ TF2MT_PSO_DEPTH = 4, TF2MT_PSO_KILL = 5 };
#define TF2MT_TEST_STRIDE 16   /* float4 slots per thread in the test harness in/out buffers */

struct tf2mt_int_consts {
    int32_t i[16][4];          /* i0..i15 */
    uint32_t b;                /* bit n = b# */
    uint32_t pad[3];
};

/* One entry per VS input register v#; built by the backend from (vertex declaration, shader reflection). */
struct tf2mt_fetch {
    uint32_t stream_type;      /* bits 0-7 stream index, bits 8-15 D3DDECLTYPE (17 = UNUSED -> (0,0,0,1)) */
    uint32_t offset;           /* stream offset + element offset, bytes, multiple of 4 */
    uint32_t stride;           /* bytes, multiple of 4; 0 = constant attribute (census: stride-0 stream 2) */
    uint32_t pad;
};

struct tf2mt_vs_driver {
    float half_pixel[4];       /* xy = (+1/viewport.w, -1/viewport.h): D3D9 pixel-centre convention (docs/semantics.md) */
    float clip_plane0[4];      /* clip-space plane (D3D9 semantics when a vertex shader is bound) */
    uint32_t stream_words[TF2MT_MAX_STREAMS];   /* bound size of each stream in 32-bit words (fetch bounds clamp) */
    struct tf2mt_fetch fetch[TF2MT_MAX_VS_INPUTS];
};

struct tf2mt_ps_driver {
    float alpha_ref;           /* D3DRS_ALPHAREF / 255 */
    float pad[3];
    float lod_bias[16];        /* D3DSAMP_MIPMAPLODBIAS per sampler (Metal samplers have no LOD bias) */
};

#ifdef __cplusplus
static_assert(sizeof(struct tf2mt_int_consts) == 272, "abi");
static_assert(sizeof(struct tf2mt_vs_driver) == 304, "abi");
static_assert(sizeof(struct tf2mt_ps_driver) == 80, "abi");
#endif
#endif
