// tf2mt backend renderer (M6, PLAN §8.4/§8.5): decodes the frontend's command stream (src/common/commands.h) and
// encodes Metal work.
//
// * State: the D3D9 render/sampler state, bindings and shader constants live here, updated in stream order.
// * Passes: one MTLRenderCommandEncoder per run of draws to the same attachments. A full-target Clear before the
//   first draw becomes the pass's load action; any other Clear draws a quad.
// * Pipelines: (VS, PS, function-constant spec, attachment formats, samples, blend, write mask) -> PSO cache.
//   Shaders are translated at creation (src/translate) and compiled asynchronously, so map loads overlap compiles.
// * Draws: dynamic vertex pulling (msl_abi.h): the fetch table comes from vertex declaration + VS reflection.
//   Constants are passed with set{Vertex,Fragment}Bytes (constant ring = M9).
// * StretchRect / Present: blit when possible, else a scaled quad with the requested filter; MSAA sources resolve
//   first.
#include "backend.h"
#include "../common/commands.h"
#include "../translate/msl.h"
#include "../translate/msl_abi.h"
#include "../translate/sm.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>
#include <cstring>
#include <string>
#include <unordered_map>
#include <condition_variable>
#include <deque>
#include <thread>
#include <mach/mach_time.h>
#include <sys/stat.h>

namespace sm = tf2mt::sm;
namespace msl = tf2mt::msl;

extern "C" CAMetalLayer *tf2mt_metal_layer(void);
extern "C" id<CAMetalDrawable> tf2mt_next_drawable(void);
extern "C" void tf2mt_present_tick(void);

namespace tf2mt::be {

struct Shader : std::enable_shared_from_this<Shader> {
    bool vs = false, ok = false;
    sm::Shader sh;
    msl::Output out;
    dispatch_group_t group = nullptr;
    id<MTLLibrary> lib = nil;        // written by the compile completion handler; read after group wait
    std::unordered_map<uint32_t, id<MTLFunction>> fns;
    std::mutex fn_mu;                // fns is used by the decoder and by background pipeline builds
    uint64_t hash = 0;               // FNV-1a of the bytecode + translator version: identity across runs (manifest)
};
struct DeclElem { uint16_t stream, offset; uint8_t type, method, usage, index; };
struct VertexDecl { std::vector<DeclElem> e; };

} // namespace tf2mt::be

namespace {
using namespace tf2mt::be;

// ---------------------------------------------------------------- D3D9 constants used here
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20,
    RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27, RS_STENCILENABLE = 52,
    RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILREF = 57,
    RS_STENCILMASK = 58, RS_STENCILWRITEMASK = 59, RS_CLIPPLANEENABLE = 152, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171,
    RS_SCISSORTESTENABLE = 174, RS_SLOPESCALEDEPTHBIAS = 175, RS_TWOSIDEDSTENCILMODE = 185, RS_CCW_STENCILFAIL = 186,
    RS_CCW_STENCILZFAIL = 187, RS_CCW_STENCILPASS = 188, RS_CCW_STENCILFUNC = 189, RS_BLENDFACTOR = 193,
    RS_SRGBWRITEENABLE = 194, RS_DEPTHBIAS = 195, RS_SEPARATEALPHABLENDENABLE = 206, RS_SRCBLENDALPHA = 207,
    RS_DESTBLENDALPHA = 208, RS_BLENDOPALPHA = 209,
};
enum { SS_ADDRESSU = 1, SS_ADDRESSV = 2, SS_ADDRESSW = 3, SS_BORDERCOLOR = 4, SS_MAGFILTER = 5, SS_MINFILTER = 6,
       SS_MIPFILTER = 7, SS_MIPMAPLODBIAS = 8, SS_MAXMIPLEVEL = 9, SS_MAXANISOTROPY = 10, SS_SRGBTEXTURE = 11, SS_N = 14 };
enum { CLEAR_TARGET = 1, CLEAR_ZBUFFER = 2, CLEAR_STENCIL = 4 };
constexpr uint32_t NSTAGE = 20;

struct SurfRef { uint32_t h = 0, face = 0, level = 0; };
struct State {
    uint32_t rs[256];
    uint32_t ss[NSTAGE][SS_N];
    uint32_t tex[NSTAGE];
    struct { uint32_t h, off, stride; } stream[16];
    uint32_t ib, ibfmt, decl, vs, ps;
    float vsf[256][4], psf[224][4];
    tf2mt_int_consts vsi, psi;
    SurfRef rt[4];
    uint32_t ds;
    uint32_t vp[4];
    float vpz[2];
    int32_t scissor[4];
    float clip[6][4];
};
State S;
uint32_t g_rt_w, g_rt_h;   // size of the current attachments

void reset_state()
{
    memset(&S, 0, sizeof S);
    uint32_t *rs = S.rs;
    rs[RS_ZENABLE] = 1; rs[RS_FILLMODE] = 3; rs[RS_ZWRITEENABLE] = 1; rs[RS_SRCBLEND] = 2; rs[RS_DESTBLEND] = 1;
    rs[RS_CULLMODE] = 3; rs[RS_ZFUNC] = 4; rs[RS_ALPHAFUNC] = 8; rs[RS_STENCILFAIL] = 1; rs[RS_STENCILZFAIL] = 1;
    rs[RS_STENCILPASS] = 1; rs[RS_STENCILFUNC] = 8; rs[RS_STENCILMASK] = 0xffffffff; rs[RS_STENCILWRITEMASK] = 0xffffffff;
    rs[RS_COLORWRITEENABLE] = 0xf; rs[RS_BLENDOP] = 1; rs[RS_CCW_STENCILFAIL] = 1; rs[RS_CCW_STENCILZFAIL] = 1;
    rs[RS_CCW_STENCILPASS] = 1; rs[RS_CCW_STENCILFUNC] = 8; rs[RS_BLENDFACTOR] = 0xffffffff; rs[RS_SRCBLENDALPHA] = 2;
    rs[RS_DESTBLENDALPHA] = 1; rs[RS_BLENDOPALPHA] = 1;
    for (auto &s : S.ss) { s[SS_ADDRESSU] = s[SS_ADDRESSV] = s[SS_ADDRESSW] = 1; s[SS_MAGFILTER] = s[SS_MINFILTER] = 1; s[SS_MAXANISOTROPY] = 1; }
    S.ibfmt = 101;   // D3DFMT_INDEX16
    S.vpz[1] = 1.0f;
}

// ---------------------------------------------------------------- logging helpers
void log_once(const char *key, const char *fmt, ...)
{
    static std::unordered_map<std::string, int> seen;
    if (seen[key]++) return;
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    tf2mt_log("%s\n", b);
}

double ms_now()
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}
// stall accounting (M8): time the encoder spent waiting on shader compiles / creating pipelines
uint64_t g_frames_total;

// Input-to-photon proxy (mouse feel): time from the moment the game is released to start frame k (its previous
// Present's SUBMIT returned; Source samples input right after) to macOS's presentedTime for frame k's drawable.
// Same clock: ms_now() and CACurrentMediaTime()/presentedTime both derive from mach_absolute_time.
double g_frame_start_ms[256];
// per-frame stage times (TF2MT_LATENCY_CSV=<unix path>): where a frame waits between start and screen
struct FrameTimes { double submit, enc, drawable, commit; float yaw, pitch; bool cam; };
FrameTimes g_ft[256];
// Queue drain (docs/mouse-input.md): macOS sometimes keeps one extra frame queued in the compositor (submit -> on screen
// one refresh longer than achievable), and since frames arrive exactly as fast as they are shown it never drains.
// presented handler tracks the achievable submit->screen time (floor) and counts consecutive late frames; present()
// then skips presenting one frame. Off by default; TF2MT_DRAIN=1 enables (see docs/mouse-input.md).
const bool g_drain_on = [] { const char *v = getenv("TF2MT_DRAIN"); return v && atoi(v) == 1; }();   // opt-in: in live play the queue re-sticks within seconds (141 drains/16 min, latency still ~31 ms)
// Detection works on half-second medians (outlier frames can't trigger it): floor = lowest median of the last 5
// minutes; 2 consecutive late windows (1 s) above floor + half a refresh request one drain (>= 2 s apart). A session
// can start stuck, so one probe drain ~15 s after the first frame (loading/menu) teaches the floor.
double g_ts_floor = 1e9, g_refresh_ms = 1000.0 / 120.0, g_last_drain_ms, g_sec_start, g_first_shown;
bool g_probe_done;
std::vector<double> g_sec_ts;
std::deque<double> g_sec_medians;
int g_late_secs;
std::atomic<bool> g_drain_request{false};
uint64_t g_drains;
FILE *latency_csv()
{
    static FILE *f = [] {
        const char *p = getenv("TF2MT_LATENCY_CSV");
        FILE *h = p ? fopen(p, "w") : nullptr;
        if (h) fprintf(h, "frame,start,game_ms,drawable_wait_ms,encode_ms,to_screen_ms,total_ms,cam_yaw,cam_pitch\n");
        return h;
    }();
    return f;
}
std::atomic<uint64_t> g_sub_presents{0};
std::mutex g_lat_mu;
std::vector<float> g_lat_samples;
const int g_game_ahead = [] { const char *v = getenv("TF2MT_GAME_AHEAD"); int n = v ? atoi(v) : 0; return n < 0 ? 0 : n > 2 ? 2 : n; }();
uint64_t g_lat_dropped;   // drawables reported as never shown (presentedTime 0)
double g_last_shown_ms;   // presentedTime of the previous shown frame
uint64_t g_shown_n, g_shown_jumps;   // shown frames, and shown intervals > 12.5 ms (a refresh showed a stale frame)
extern double g_encode_ms;   // backend CPU time decoding/encoding command streams since the last ledger line
double g_stall_ms_frame;
uint64_t g_stall_events, g_pso_misses;

uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p; uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}
template <class K> struct BytesHash { size_t operator()(const K &k) const { return (size_t)fnv(&k, sizeof k); } };
template <class K> struct BytesEq { bool operator()(const K &a, const K &b) const { return !memcmp(&a, &b, sizeof a); } };

// ---------------------------------------------------------------- shaders
MTLCompileOptions *g_opts;

std::shared_ptr<Shader> make_shader(const uint32_t *code, uint32_t dwords, std::string &err)
{
    auto sh = std::make_shared<Shader>();
    if (!sm::decode(code, dwords, sh->sh, err)) return nullptr;
    sh->vs = sh->sh.stage == sm::Stage::Vertex;
    msl::Options mo;
    if (!msl::translate(sh->sh, mo, sh->out, err)) return nullptr;
    sh->ok = true;
    sh->group = dispatch_group_create();
    dispatch_group_enter(sh->group);
    NSString *src = [[NSString alloc] initWithBytes:sh->out.source.data() length:sh->out.source.size() encoding:NSUTF8StringEncoding];
    std::shared_ptr<Shader> keep = sh;
    [tf2mt_metal_device() newLibraryWithSource:src options:g_opts completionHandler:^(id<MTLLibrary> lib, NSError *e) {
        keep->lib = lib;
        if (!lib) tf2mt_log("shader compile failed (%s): %s\n", sm::version_string(keep->sh).c_str(), e.localizedDescription.UTF8String);
        dispatch_group_leave(keep->group);
    }];
    return sh;
}

id<MTLFunction> shader_function(Shader &sh, int alpha_func, uint32_t clip_mask, bool background = false)
{
    uint32_t key = (uint32_t)alpha_func | clip_mask << 8;
    std::lock_guard<std::mutex> fl(sh.fn_mu);
    auto it = sh.fns.find(key);
    if (it != sh.fns.end()) return it->second;
    double t0 = ms_now();
    dispatch_group_wait(sh.group, DISPATCH_TIME_FOREVER);
    double waited = ms_now() - t0;
    if (waited > 2.0 && !background) { g_stall_ms_frame += waited; g_stall_events++; tf2mt_log("stall: frame %llu waited %.1f ms for shader compile\n", (unsigned long long)g_frames_total, waited); }
    id<MTLFunction> fn = nil;
    if (sh.lib) {
        MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        [cv setConstantValue:&alpha_func type:MTLDataTypeInt atIndex:TF2MT_FC_ALPHA_FUNC];
        [cv setConstantValue:&clip_mask type:MTLDataTypeUInt atIndex:TF2MT_FC_CLIP_MASK];
        NSError *e = nil;
        fn = [sh.lib newFunctionWithName:[NSString stringWithUTF8String:sh.out.entry.c_str()] constantValues:cv error:&e];
        if (!fn) tf2mt_log("specialisation failed: %s\n", e.localizedDescription.UTF8String);
    }
    sh.fns[key] = fn;
    return fn;
}

// ---------------------------------------------------------------- texture views
MTLPixelFormat srgb_of(MTLPixelFormat f)
{
    switch (f) {
    case MTLPixelFormatBGRA8Unorm: return MTLPixelFormatBGRA8Unorm_sRGB;
    case MTLPixelFormatRGBA8Unorm: return MTLPixelFormatRGBA8Unorm_sRGB;
    case MTLPixelFormatBC1_RGBA: return MTLPixelFormatBC1_RGBA_sRGB;
    case MTLPixelFormatBC2_RGBA: return MTLPixelFormatBC2_RGBA_sRGB;
    case MTLPixelFormatBC3_RGBA: return MTLPixelFormatBC3_RGBA_sRGB;
    default: return f;
    }
}
id<MTLTexture> sampling_view(Obj &o, bool srgb)
{
    srgb = srgb && o.fmt->srgb_view;
    id<MTLTexture> __strong &v = srgb ? o.view_srgb : o.view_linear;
    if (v) return v;
    if (!srgb && !swizzled(o.fmt)) return v = o.tex;
    NSUInteger slices = o.type == TF2MT_TEX_CUBE ? 6 : 1;
    v = [o.tex newTextureViewWithPixelFormat:srgb ? srgb_of(o.fmt->pf) : o.fmt->pf textureType:o.tex.textureType
                                       levels:NSMakeRange(0, o.tex.mipmapLevelCount) slices:NSMakeRange(0, slices)
                                      swizzle:o.fmt->swz];
    return v;
}
id<MTLTexture> rt_view(Obj &o, bool srgb)
{
    srgb = srgb && o.fmt->srgb_view;
    if (!srgb) return o.tex;
    if (o.rt_views.empty()) {
        NSUInteger slices = o.type == TF2MT_TEX_CUBE ? 6 : 1;
        o.rt_views.push_back([o.tex newTextureViewWithPixelFormat:srgb_of(o.fmt->pf) textureType:o.tex.textureType
                                                           levels:NSMakeRange(0, o.tex.mipmapLevelCount) slices:NSMakeRange(0, slices)]);
    }
    return o.rt_views[0];
}

// ---------------------------------------------------------------- caches: samplers, depth-stencil, pipelines
struct SamplerKey { uint32_t v[7]; };
std::unordered_map<SamplerKey, id<MTLSamplerState>, BytesHash<SamplerKey>, BytesEq<SamplerKey>> g_samplers;
MTLSamplerAddressMode addr(uint32_t a)
{
    switch (a) {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}
id<MTLSamplerState> sampler_for(const uint32_t *ss)
{
    SamplerKey k = {{ss[SS_ADDRESSU], ss[SS_ADDRESSV], ss[SS_ADDRESSW], ss[SS_MAGFILTER] | ss[SS_MINFILTER] << 4 | ss[SS_MIPFILTER] << 8,
                     ss[SS_MAXANISOTROPY], ss[SS_MAXMIPLEVEL], ss[SS_BORDERCOLOR]}};
    auto it = g_samplers.find(k);
    if (it != g_samplers.end()) return it->second;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.sAddressMode = addr(k.v[0]); d.tAddressMode = addr(k.v[1]); d.rAddressMode = addr(k.v[2]);
    uint32_t mag = ss[SS_MAGFILTER], min = ss[SS_MINFILTER], mip = ss[SS_MIPFILTER];
    d.magFilter = mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = mip == 0 ? MTLSamplerMipFilterNotMipmapped : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    if (min == 3 || mag == 3) d.maxAnisotropy = std::min(16u, std::max(1u, ss[SS_MAXANISOTROPY]));
    d.lodMinClamp = (float)ss[SS_MAXMIPLEVEL];
    uint32_t bc = ss[SS_BORDERCOLOR];
    d.borderColor = bc == 0 ? MTLSamplerBorderColorTransparentBlack : (bc >> 24) == 0xff && (bc & 0xffffff) == 0 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    id<MTLSamplerState> s = [tf2mt_metal_device() newSamplerStateWithDescriptor:d];
    g_samplers[k] = s;
    return s;
}

MTLCompareFunction cmp(uint32_t c)
{
    static const MTLCompareFunction t[] = {MTLCompareFunctionAlways, MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways};
    return c <= 8 ? t[c] : MTLCompareFunctionAlways;
}
MTLStencilOperation sop(uint32_t o)
{
    static const MTLStencilOperation t[] = {MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationZero,
        MTLStencilOperationReplace, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp,
        MTLStencilOperationInvert, MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap};
    return o <= 8 ? t[o] : MTLStencilOperationKeep;
}

struct DssKey { uint32_t z, s, front, back, masks; };
std::unordered_map<DssKey, id<MTLDepthStencilState>, BytesHash<DssKey>, BytesEq<DssKey>> g_dss;
id<MTLDepthStencilState> dss_make(const DssKey &k)
{
    auto it = g_dss.find(k);
    if (it != g_dss.end()) return it->second;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = cmp(k.z & 0xf);
    d.depthWriteEnabled = (k.z >> 4) & 1;
    if (k.s) {
        auto mk = [&](uint32_t p) {
            MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
            sd.stencilFailureOperation = sop(p & 0xf); sd.depthFailureOperation = sop((p >> 4) & 0xf);
            sd.depthStencilPassOperation = sop((p >> 8) & 0xf); sd.stencilCompareFunction = cmp((p >> 12) & 0xf);
            sd.readMask = k.masks & 0xff; sd.writeMask = (k.masks >> 8) & 0xff;
            return sd;
        };
        d.frontFaceStencil = mk(k.front);
        d.backFaceStencil = mk(k.back);
    }
    id<MTLDepthStencilState> s = [tf2mt_metal_device() newDepthStencilStateWithDescriptor:d];
    g_dss[k] = s;
    return s;
}
id<MTLDepthStencilState> dss_for_state()
{
    const uint32_t *rs = S.rs;
    DssKey k = {};
    bool zen = rs[RS_ZENABLE] != 0;
    k.z = (zen ? rs[RS_ZFUNC] : 8) | (zen && rs[RS_ZWRITEENABLE] ? 0x10 : 0);
    if (rs[RS_STENCILENABLE]) {
        k.s = 1;
        k.front = rs[RS_STENCILFAIL] | rs[RS_STENCILZFAIL] << 4 | rs[RS_STENCILPASS] << 8 | rs[RS_STENCILFUNC] << 12;
        k.back = rs[RS_TWOSIDEDSTENCILMODE]
            ? rs[RS_CCW_STENCILFAIL] | rs[RS_CCW_STENCILZFAIL] << 4 | rs[RS_CCW_STENCILPASS] << 8 | rs[RS_CCW_STENCILFUNC] << 12
            : k.front;
        k.masks = (rs[RS_STENCILMASK] & 0xff) | (rs[RS_STENCILWRITEMASK] & 0xff) << 8;
    }
    return dss_make(k);
}

MTLBlendFactor bfac(uint32_t b)
{
    switch (b) {
    case 1: return MTLBlendFactorZero;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}
MTLBlendOperation bop(uint32_t o)
{
    switch (o) {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

struct PsoKey {
    Shader *vs, *ps;
    uint32_t vspec, pspec;
    uint32_t color, depth, stencil, samples;
    uint32_t blend;      // enable | src<<1 | dst<<5 | op<<9 | srca<<12 | dsta<<16 | opa<<20 | separate<<23
    uint32_t wmask;
};
std::unordered_map<PsoKey, id<MTLRenderPipelineState>, BytesHash<PsoKey>, BytesEq<PsoKey>> g_psos;
extern id<MTLLibrary> g_internal;
// Build a pipeline (decoder thread or background prewarm; touches no shared tables).
id<MTLRenderPipelineState> build_pso(const PsoKey &k, bool background)
{
    id<MTLFunction> vf = shader_function(*k.vs, 8, k.vspec, background);
    id<MTLFunction> ff = k.ps ? shader_function(*k.ps, (int)k.pspec, 0, background) : [g_internal newFunctionWithName:@"ffp_ps"];
    id<MTLRenderPipelineState> p = nil;
    if (vf && ff) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vf; d.fragmentFunction = ff;
        d.rasterSampleCount = k.samples;
        d.depthAttachmentPixelFormat = (MTLPixelFormat)k.depth;
        d.stencilAttachmentPixelFormat = (MTLPixelFormat)k.stencil;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
        c.pixelFormat = (MTLPixelFormat)k.color;
        if (k.color) {
            uint32_t b = k.blend;
            c.blendingEnabled = b & 1;
            uint32_t src = (b >> 1) & 0xf, dst = (b >> 5) & 0xf, srca = (b >> 12) & 0xf, dsta = (b >> 16) & 0xf;
            auto both = [](uint32_t &s, uint32_t &d) {   // D3DBLEND_BOTHSRCALPHA / BOTHINVSRCALPHA set both factors
                if (s == 12) { s = 5; d = 6; } else if (s == 13) { s = 6; d = 5; }
            };
            both(src, dst); both(srca, dsta);
            c.sourceRGBBlendFactor = bfac(src); c.destinationRGBBlendFactor = bfac(dst);
            c.rgbBlendOperation = bop((b >> 9) & 7);
            bool sep = (b >> 23) & 1;
            c.sourceAlphaBlendFactor = bfac(sep ? srca : src); c.destinationAlphaBlendFactor = bfac(sep ? dsta : dst);
            c.alphaBlendOperation = bop(sep ? (b >> 20) & 7 : (b >> 9) & 7);
            MTLColorWriteMask m = MTLColorWriteMaskNone;
            if (k.wmask & 1) m |= MTLColorWriteMaskRed;
            if (k.wmask & 2) m |= MTLColorWriteMaskGreen;
            if (k.wmask & 4) m |= MTLColorWriteMaskBlue;
            if (k.wmask & 8) m |= MTLColorWriteMaskAlpha;
            c.writeMask = m;
        }
        NSError *e = nil;
        p = [tf2mt_metal_device() newRenderPipelineStateWithDescriptor:d error:&e];
        if (!p) tf2mt_log("PSO creation failed: %s\n", e.localizedDescription.UTF8String);
    }
    return p;
}

// ---------------------------------------------------------------- M8 anti-stutter: manifest, prewarm, miss policy
// Every pipeline created is appended to a persistent manifest (shader bytecode hashes + state). When a later run
// creates both shaders of a manifest entry (normally during the map load), the pipeline is built on a background
// queue; finished builds are adopted by the decoder at the next lookup. On a miss that is still pending or unknown:
// translucent draws (blending on: particles, decals, sprites) are skipped until the background build lands
// (PLAN §8.5 "skip non-critical"); opaque draws build synchronously.
struct PersistKey { uint64_t vs, ps; uint32_t vspec, pspec, color, depth, stencil, samples, blend, wmask; };
std::vector<PersistKey> g_manifest;
std::unordered_map<PersistKey, int, BytesHash<PersistKey>, BytesEq<PersistKey>> g_manifest_set;
std::unordered_map<uint64_t, std::weak_ptr<Shader>> g_live;   // bytecode hash -> live shader
FILE *g_manifest_file;
struct Pending { std::atomic<bool> done{false}, started{false}, promoted{false}; id<MTLRenderPipelineState> pso = nil; std::weak_ptr<Shader> vs, ps; };
std::unordered_map<PsoKey, std::shared_ptr<Pending>, BytesHash<PsoKey>, BytesEq<PsoKey>> g_building;
dispatch_queue_t g_build_q, g_urgent_q;
dispatch_semaphore_t g_build_slots, g_urgent_slots;   // prewarm and on-demand builds have separate pools
uint64_t g_prewarmed, g_skipped_translucent, g_adopted;
bool g_defer = true;
bool g_no_lodbias, g_no_enccache;   // diagnostics: TF2MT_NO_LODBIAS=1, TF2MT_NO_ENCCACHE=1   // TF2MT_NO_DEFER=1: never skip draws (deterministic output for golden comparisons)

PersistKey persist(const PsoKey &k)
{
    PersistKey p;
    memset(&p, 0, sizeof p);
    p.vs = k.vs->hash; p.ps = k.ps ? k.ps->hash : 0; p.vspec = k.vspec; p.pspec = k.pspec; p.color = k.color;
    p.depth = k.depth; p.stencil = k.stencil; p.samples = k.samples; p.blend = k.blend; p.wmask = k.wmask;
    return p;
}
void manifest_add(const PsoKey &k)
{
    PersistKey p = persist(k);
    if (g_manifest_set.count(p)) return;
    g_manifest_set[p] = 1;
    g_manifest.push_back(p);
    if (g_manifest_file) { fwrite(&p, sizeof p, 1, g_manifest_file); fflush(g_manifest_file); }
}

std::string cache_dir()
{
    if (const char *d = getenv("TF2MT_CACHE_DIR")) return d;
    if (const char *h = getenv("TF2_HOME")) return std::string(h) + "/cache/tf2mt";
    const char *home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/Games/tf2/cache/tf2mt";
}
void manifest_load()
{
    std::string dir = cache_dir();
    mkdir(dir.c_str(), 0755);
    std::string path = dir + "/pipelines-v1.bin";   // bump the version when the translator's output changes
    if (FILE *f = fopen(path.c_str(), "rb")) {
        PersistKey p;
        while (fread(&p, sizeof p, 1, f) == 1) if (!g_manifest_set.count(p)) { g_manifest_set[p] = 1; g_manifest.push_back(p); }
        fclose(f);
    }
    g_manifest_file = fopen(path.c_str(), "ab");
    g_build_q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
    g_urgent_q = dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0);
    g_build_slots = dispatch_semaphore_create(3);    // prewarm (manifest) builds
    g_urgent_slots = dispatch_semaphore_create(4);   // pipelines a draw is waiting for: never queued behind prewarm
    if (const char *v = getenv("TF2MT_NO_DEFER")) g_defer = *v != '1';
    if (const char *v = getenv("TF2MT_NO_LODBIAS")) g_no_lodbias = *v == '1';
    if (const char *v = getenv("TF2MT_NO_ENCCACHE")) g_no_enccache = *v == '1';
    tf2mt_log("pipeline manifest: %zu entries (%s)\n", g_manifest.size(), path.c_str());
}

// Pipeline build pool: a fixed set of worker threads (blocking GCD workers on a semaphore starved the on-demand
// builds behind ~1,500 prewarm jobs). On-demand jobs (a draw is waiting) always run before prewarm jobs.
struct BuildJob { PsoKey k; std::shared_ptr<Pending> pend; std::shared_ptr<Shader> vs, ps; };
std::mutex g_bmu;
std::condition_variable g_bcv;
std::deque<BuildJob> g_urgentq, g_prewarmq;
void build_worker()
{
    for (;;) {
        BuildJob j;
        {
            std::unique_lock<std::mutex> l(g_bmu);
            g_bcv.wait(l, [] { return !g_urgentq.empty() || !g_prewarmq.empty(); });
            auto &q = !g_urgentq.empty() ? g_urgentq : g_prewarmq;
            j = std::move(q.front());
            q.pop_front();
        }
        if (j.pend->started.exchange(true)) continue;   // the same job queued twice (prewarm, then on demand)
        @autoreleasepool { j.pend->pso = build_pso(j.k, true); }
        j.pend->done = true;
    }
}
void build_async(const PsoKey &k, const std::shared_ptr<Shader> &vs, const std::shared_ptr<Shader> &ps, bool urgent = false)
{
    static bool started;
    if (!started) { started = true; for (int i = 0; i < 4; i++) std::thread(build_worker).detach(); }
    auto it = g_building.find(k);
    if (g_psos.count(k)) return;
    if (it != g_building.end()) {   // already queued: promote to the on-demand queue if a draw now needs it
        if (urgent && !it->second->started && !it->second->promoted.exchange(true)) {
            std::lock_guard<std::mutex> l(g_bmu);
            g_urgentq.push_front({k, it->second, vs, ps});
            g_bcv.notify_one();
        }
        return;
    }
    auto pend = std::make_shared<Pending>();
    pend->vs = vs; pend->ps = ps;
    g_building[k] = pend;
    std::lock_guard<std::mutex> l(g_bmu);
    (urgent ? g_urgentq : g_prewarmq).push_back({k, pend, vs, ps});
    g_bcv.notify_one();
}

// Shader created: prewarm every manifest pipeline whose shaders are now all alive.
void prewarm_for(const std::shared_ptr<Shader> &sh)
{
    for (const PersistKey &p : g_manifest) {
        if (p.vs != sh->hash && p.ps != sh->hash) continue;
        auto vs = g_live.count(p.vs) ? g_live[p.vs].lock() : nullptr;
        std::shared_ptr<Shader> ps = p.ps ? (g_live.count(p.ps) ? g_live[p.ps].lock() : nullptr) : nullptr;
        if (!vs || (p.ps && !ps)) continue;
        PsoKey k;
        memset(&k, 0, sizeof k);
        k.vs = vs.get(); k.ps = ps.get(); k.vspec = p.vspec; k.pspec = p.pspec; k.color = p.color; k.depth = p.depth;
        k.stencil = p.stencil; k.samples = p.samples; k.blend = p.blend; k.wmask = p.wmask;
        if (!g_psos.count(k) && !g_building.count(k)) { build_async(k, vs, ps); g_prewarmed++; }
    }
}

// live shared_ptr for a raw pointer (via g_live)
std::shared_ptr<Shader> owner_of(Shader *s)
{
    // TF2 creates identical shaders more than once (same hash), so the owner is found through the object itself
    std::weak_ptr<Shader> w = s->weak_from_this();
    return w.lock();
}


id<MTLRenderPipelineState> pso_get(const PsoKey &k, bool translucent, bool *skip)
{
    *skip = false;
    auto it = g_psos.find(k);
    if (it != g_psos.end()) return it->second;
    auto pit = g_building.find(k);
    if (pit != g_building.end()) {
        if (pit->second->done) {   // adopt a finished background build
            id<MTLRenderPipelineState> p = pit->second->pso;
            g_building.erase(pit);
            g_psos[k] = p;
            g_adopted++;
            manifest_add(k);
            return p;
        }
        if (g_defer) {
            // a queued prewarm job may sit behind others: promote it to the on-demand queue
            if (!pit->second->started) { auto vs = owner_of(k.vs), ps = k.ps ? owner_of(k.ps) : nullptr;
                if (vs && (!k.ps || ps)) build_async(k, vs, ps, true); }
            *skip = true; g_skipped_translucent++; return nil;
        }
        g_building.erase(pit);   // deterministic mode: build now (the background copy is discarded)
    } else if (g_defer) {
        // DXVK-async-style policy (owner's DXVK uses async): skip the draw until its pipeline is built in the
        // background, instead of stalling the frame (19 ms median per pipeline on a cold Medium preset)
        auto vs = owner_of(k.vs), ps = k.ps ? owner_of(k.ps) : nullptr;
        if (vs && (!k.ps || ps)) { build_async(k, vs, ps, true); *skip = true; g_skipped_translucent++; return nil; }
    }
    g_pso_misses++;
    double t0 = ms_now();
    id<MTLRenderPipelineState> p = build_pso(k, false);
    double dt = ms_now() - t0;
    if (dt > 2.0) { g_stall_ms_frame += dt; g_stall_events++; tf2mt_log("stall: frame %llu PSO miss %.1f ms (spec+create)\n", (unsigned long long)g_frames_total, dt); }
    g_psos[k] = p;
    if (p) manifest_add(k);
    return p;
}

// ---------------------------------------------------------------- internal pipelines: clear quad, scaled copy
id<MTLLibrary> g_internal = nil;
const char *kInternalSrc = R"MSL(#include <metal_stdlib>
using namespace metal;
struct ClearArgs { float4 rect; float4 color; float z; float3 pad; };
struct ClearOut { float4 pos [[position]]; };
vertex ClearOut clear_vs(uint vid [[vertex_id]], constant ClearArgs &a [[buffer(0)]])
{
    float2 uv = float2(vid & 1, vid >> 1);
    ClearOut o; o.pos = float4(mix(a.rect.xy, a.rect.zw, uv), a.z, 1.0); return o;
}
fragment float4 clear_fs(constant ClearArgs &a [[buffer(0)]]) { return a.color; }
struct CopyArgs { float4 rect; float4 uv; };
struct CopyOut { float4 pos [[position]]; float2 uv; };
vertex CopyOut copy_vs(uint vid [[vertex_id]], constant CopyArgs &a [[buffer(0)]])
{
    float2 t = float2(vid & 1, vid >> 1);
    CopyOut o; o.pos = float4(mix(a.rect.xy, a.rect.zw, t), 0.0, 1.0); o.uv = mix(a.uv.xy, a.uv.zw, t); return o;
}
fragment float4 copy_fs(CopyOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{ return t.sample(s, in.uv, level(0.0)); }
// D3D9 draw with a vertex shader and no pixel shader: fixed-function pixel stage with default texture-stage states
// and no texture = interpolated diffuse colour (census: TSS vestigial; these draws are depth/occlusion passes)
struct FfpIn { float4 c0 [[user(c0)]]; };
fragment float4 ffp_ps(FfpIn in [[stage_in]]) { return in.c0; }
)MSL";

struct InternalKey { uint32_t kind, color, depth, stencil, samples, wmask; };
std::unordered_map<InternalKey, id<MTLRenderPipelineState>, BytesHash<InternalKey>, BytesEq<InternalKey>> g_internal_psos;
id<MTLRenderPipelineState> internal_pso(uint32_t kind, MTLPixelFormat color, MTLPixelFormat depth, MTLPixelFormat stencil,
                                        uint32_t samples, uint32_t wmask)
{
    InternalKey k = {kind, (uint32_t)color, (uint32_t)depth, (uint32_t)stencil, samples, wmask};
    auto it = g_internal_psos.find(k);
    if (it != g_internal_psos.end()) return it->second;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_internal newFunctionWithName:kind ? @"copy_vs" : @"clear_vs"];
    d.fragmentFunction = [g_internal newFunctionWithName:kind ? @"copy_fs" : @"clear_fs"];
    d.rasterSampleCount = samples;
    d.colorAttachments[0].pixelFormat = color;
    d.colorAttachments[0].writeMask = wmask ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
    d.depthAttachmentPixelFormat = depth;
    d.stencilAttachmentPixelFormat = stencil;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [tf2mt_metal_device() newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) tf2mt_log("internal PSO failed: %s\n", e.localizedDescription.UTF8String);
    g_internal_psos[k] = p;
    return p;
}

// ---------------------------------------------------------------- queries (occlusion = visibility counting, event = serial)
id<MTLBuffer> g_vis;                         // TF2MT_QUERY_SLOTS x uint64 sample counts
uint64_t g_qserial[TF2MT_QUERY_SLOTS];       // submission serial that completes the query; UINT64_MAX = still open
uint32_t g_qgen[TF2MT_QUERY_SLOTS];          // generation of the last decoded BEGIN/END/EVENT on that slot
std::atomic<bool> g_flush_requested{false};  // GetData(FLUSH) for a query the encoder has not reached yet
int32_t g_active_q = -1;                     // occlusion query collecting samples
int32_t g_enc_q = -2;                        // visibility mode last set on the open encoder

id<MTLTexture> g_dummy2d, g_dummycube, g_dummy3d;
id<MTLBuffer> g_dummybuf;
id<MTLSamplerState> g_linear, g_point;

void init_internal()
{
    if (g_internal) return;
    manifest_load();
    id<MTLDevice> dev = tf2mt_metal_device();
    g_opts = [MTLCompileOptions new];
    if (@available(macOS 15.0, *)) g_opts.mathMode = MTLMathModeRelaxed;   // fast math that keeps INF/NaN (ADR-003)
    else g_opts.fastMathEnabled = NO;
    if (@available(macOS 14.0, *)) g_opts.languageVersion = MTLLanguageVersion3_1;
    NSError *e = nil;
    g_internal = [dev newLibraryWithSource:[NSString stringWithUTF8String:kInternalSrc] options:g_opts error:&e];
    if (!g_internal) tf2mt_log("internal shaders failed: %s\n", e.localizedDescription.UTF8String);
    const uint8_t black[4] = {0, 0, 0, 255};   // D3D9: sampling an unbound stage returns (0, 0, 0, 1)
    auto dummy = [&](MTLTextureType t) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = t; d.pixelFormat = MTLPixelFormatRGBA8Unorm; d.width = d.height = 1; d.depth = 1;
        d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [dev newTextureWithDescriptor:d];
        for (NSUInteger s = 0; s < (t == MTLTextureTypeCube ? 6u : 1u); s++)
            [tex replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:s withBytes:black bytesPerRow:4 bytesPerImage:4];
        return tex;
    };
    g_dummy2d = dummy(MTLTextureType2D); g_dummycube = dummy(MTLTextureTypeCube); g_dummy3d = dummy(MTLTextureType3D);
    g_dummybuf = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    g_vis = [dev newBufferWithLength:TF2MT_QUERY_SLOTS * 8 options:MTLResourceStorageModeShared];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    g_linear = [dev newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    g_point = [dev newSamplerStateWithDescriptor:sd];
    reset_state();
}

// ---------------------------------------------------------------- render passes
struct Attach { id<MTLTexture> color; uint32_t cslice, clevel; id<MTLTexture> depth; };
Attach g_att;
id<MTLRenderCommandEncoder> g_enc;
// M9: state last set on the open encoder, so unchanged state is not re-sent per draw. Reset on every new encoder and
// after internal draws (clear quads) that change it behind the cache's back.
struct EncCache {
    __unsafe_unretained id<MTLRenderPipelineState> pso;
    __unsafe_unretained id<MTLDepthStencilState> dss;
    uint32_t sref, cull, fill, bf, valid;
    float bias, slope;
    MTLViewport vp;
    MTLScissorRect sc;
    __unsafe_unretained id<MTLBuffer> vb[4];
    __unsafe_unretained id<MTLTexture> tex[16];
    __unsafe_unretained id<MTLSamplerState> smp[16];
    tf2mt_vs_driver vdrv;
    tf2mt_ps_driver pdrv;
};
EncCache g_ec;
bool g_vsf_dirty = true, g_psf_dirty = true, g_vsi_dirty = true, g_psi_dirty = true;   // constants changed since bound
void enc_cache_reset() { memset(&g_ec, 0, sizeof g_ec); g_vsf_dirty = g_psf_dirty = g_vsi_dirty = g_psi_dirty = true; }
struct { bool color, depth, stencil; float rgba[4]; float z; uint32_t st; } g_pending;
uint64_t g_draws, g_skipped;

bool has_stencil(MTLPixelFormat f) { return f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatX32_Stencil8; }

// Current attachments from state (sRGB view when SRGBWRITEENABLE).
bool attachments_from_state(Attach &a)
{
    a = Attach{};
    if (Obj *o = lookup(S.rt[0].h); o && o->kind == K_TEXTURE && o->tex) {
        a.color = rt_view(*o, S.rs[RS_SRGBWRITEENABLE] != 0);
        a.cslice = S.rt[0].face; a.clevel = S.rt[0].level;
    }
    if (Obj *o = lookup(S.ds); o && o->kind == K_TEXTURE && o->tex && o->fmt->depth) a.depth = o->tex;
    return a.color || a.depth;
}

void end_encoder()
{
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
}

// Make sure an encoder for `a` is open (applying any pending clear as load actions).
id<MTLRenderCommandEncoder> encoder_for(const Attach &a)
{
    if (g_enc && a.color == g_att.color && a.cslice == g_att.cslice && a.clevel == g_att.clevel && a.depth == g_att.depth) return g_enc;
    end_encoder();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (a.color) {
        MTLRenderPassColorAttachmentDescriptor *c = rp.colorAttachments[0];
        c.texture = a.color; c.level = a.clevel; c.slice = a.cslice;
        c.loadAction = g_pending.color ? MTLLoadActionClear : MTLLoadActionLoad;
        c.clearColor = MTLClearColorMake(g_pending.rgba[0], g_pending.rgba[1], g_pending.rgba[2], g_pending.rgba[3]);
        c.storeAction = MTLStoreActionStore;
        g_rt_w = std::max<uint32_t>(1, (uint32_t)a.color.width >> a.clevel); g_rt_h = std::max<uint32_t>(1, (uint32_t)a.color.height >> a.clevel);
    }
    if (a.depth) {
        rp.depthAttachment.texture = a.depth;
        rp.depthAttachment.loadAction = g_pending.depth ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = g_pending.z;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (has_stencil(a.depth.pixelFormat)) {
            rp.stencilAttachment.texture = a.depth;
            rp.stencilAttachment.loadAction = g_pending.stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = g_pending.st;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!a.color) { g_rt_w = (uint32_t)a.depth.width; g_rt_h = (uint32_t)a.depth.height; }
    }
    g_pending.color = g_pending.depth = g_pending.stencil = false;
    rp.visibilityResultBuffer = g_vis;
    g_enc = [frame_cb_locked() renderCommandEncoderWithDescriptor:rp];
    g_enc_q = -2;
    enc_cache_reset();
    [g_enc setFrontFacingWinding:MTLWindingClockwise];
    g_att = a;
    return g_enc;
}

uint32_t samples_of(const Attach &a) { return (uint32_t)(a.color ? a.color.sampleCount : a.depth ? a.depth.sampleCount : 1); }

// ---------------------------------------------------------------- draws
uint32_t index_count(uint32_t prim, uint32_t n, MTLPrimitiveType *t)
{
    switch (prim) {
    case 1: *t = MTLPrimitiveTypePoint; return n;
    case 2: *t = MTLPrimitiveTypeLine; return n * 2;
    case 3: *t = MTLPrimitiveTypeLineStrip; return n + 1;
    case 4: *t = MTLPrimitiveTypeTriangle; return n * 3;
    case 5: *t = MTLPrimitiveTypeTriangleStrip; return n + 2;
    default: return 0;   // fans: not in the census
    }
}

void set_viewport_scissor(id<MTLRenderCommandEncoder> enc)
{
    MTLViewport v = {(double)S.vp[0], (double)S.vp[1], (double)S.vp[2], (double)S.vp[3], S.vpz[0], S.vpz[1]};
    if (memcmp(&v, &g_ec.vp, sizeof v)) { [enc setViewport:v]; g_ec.vp = v; }
    int32_t l = 0, t = 0, r = (int32_t)g_rt_w, b = (int32_t)g_rt_h;
    if (S.rs[RS_SCISSORTESTENABLE]) {
        l = std::max(l, S.scissor[0]); t = std::max(t, S.scissor[1]);
        r = std::min(r, S.scissor[2]); b = std::min(b, S.scissor[3]);
    }
    if (r <= l || b <= t) { l = t = 0; r = b = 1; }   // empty scissor: callers skip the draw
    MTLScissorRect sr = {(NSUInteger)l, (NSUInteger)t, (NSUInteger)(r - l), (NSUInteger)(b - t)};
    if (memcmp(&sr, &g_ec.sc, sizeof sr)) { [enc setScissorRect:sr]; g_ec.sc = sr; }
}

bool scissor_empty()
{
    if (!S.rs[RS_SCISSORTESTENABLE]) return false;
    return std::min<int32_t>(S.scissor[2], (int32_t)g_rt_w) <= std::max(0, S.scissor[0])
        || std::min<int32_t>(S.scissor[3], (int32_t)g_rt_h) <= std::max(0, S.scissor[1]);
}

void draw(bool indexed, const uint32_t *p)
{
    uint32_t prim = p[0];
    Obj *vso = lookup(S.vs), *pso_ = lookup(S.ps);
    bool ffp = !S.ps;   // no pixel shader: fixed-function pixel stage (see ffp_ps)
    if (!vso || vso->kind != K_SHADER || !vso->shader || (!ffp && (!pso_ || pso_->kind != K_SHADER || !pso_->shader))) {
        g_skipped++;
        static uint64_t n;
        if (n++ < 8) tf2mt_log("draw skipped: vs %#x (%s) ps %#x (%s)\n", S.vs, !S.vs ? "unset" : vso ? (vso->shader ? "ok" : "rejected") : "stale handle",
                               S.ps, !S.ps ? "unset" : pso_ ? (pso_->shader ? "ok" : "rejected") : "stale handle");
        return;
    }
    Shader &vs = *vso->shader;
    Shader *ps = ffp ? nullptr : pso_->shader.get();
    Attach a;
    if (!attachments_from_state(a)) { g_skipped++; log_once("noatt", "draw skipped: no attachments"); return; }
    MTLPrimitiveType mt;
    uint32_t n = index_count(prim, indexed ? p[5] : p[2], &mt);
    if (!n) { g_skipped++; log_once("prim", "draw skipped: primitive type %u", prim); return; }

    id<MTLRenderCommandEncoder> enc = encoder_for(a);
    if (scissor_empty()) return;
    const uint32_t *rs = S.rs;
    PsoKey k;
    memset(&k, 0, sizeof k);
    k.vs = &vs; k.ps = ps;
    k.vspec = rs[RS_CLIPPLANEENABLE] & 1;
    k.pspec = rs[RS_ALPHATESTENABLE] ? rs[RS_ALPHAFUNC] : 8;
    k.color = a.color ? (uint32_t)a.color.pixelFormat : 0;
    k.depth = a.depth ? (uint32_t)a.depth.pixelFormat : 0;
    k.stencil = a.depth && has_stencil(a.depth.pixelFormat) ? (uint32_t)a.depth.pixelFormat : 0;
    k.samples = samples_of(a);
    k.blend = (rs[RS_ALPHABLENDENABLE] ? 1 : 0) | (rs[RS_SRCBLEND] & 0xf) << 1 | (rs[RS_DESTBLEND] & 0xf) << 5 | (rs[RS_BLENDOP] & 7) << 9
            | (rs[RS_SRCBLENDALPHA] & 0xf) << 12 | (rs[RS_DESTBLENDALPHA] & 0xf) << 16 | (rs[RS_BLENDOPALPHA] & 7) << 20
            | (rs[RS_SEPARATEALPHABLENDENABLE] ? 1u : 0u) << 23;
    if (!(k.blend & 1)) k.blend = 0;
    k.wmask = rs[RS_COLORWRITEENABLE] & 0xf;
    bool skip;
    id<MTLRenderPipelineState> pso = pso_get(k, (k.blend & 1) != 0, &skip);
    if (skip) return;   // translucent draw whose pipeline is still compiling: dropped for this frame (M8 policy)
    if (!pso) { g_skipped++; return; }

    if (g_no_enccache) enc_cache_reset();
    bool first = !g_ec.valid;
    g_ec.valid = 1;
    if (first || g_ec.pso != pso) { [enc setRenderPipelineState:pso]; g_ec.pso = pso; }
    id<MTLDepthStencilState> dss = dss_for_state();
    if (first || g_ec.dss != dss) { [enc setDepthStencilState:dss]; g_ec.dss = dss; }
    uint32_t sref = rs[RS_STENCILREF] & 0xff;
    if (first || g_ec.sref != sref) { [enc setStencilReferenceValue:sref]; g_ec.sref = sref; }
    uint32_t cull = rs[RS_CULLMODE];
    if (first || g_ec.cull != cull) { [enc setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone]; g_ec.cull = cull; }
    uint32_t fill = rs[RS_FILLMODE];
    if (first || g_ec.fill != fill) { [enc setTriangleFillMode:fill == 2 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill]; g_ec.fill = fill; }
    float bias, slope;
    memcpy(&bias, &rs[RS_DEPTHBIAS], 4); memcpy(&slope, &rs[RS_SLOPESCALEDEPTHBIAS], 4);
    if (first || g_ec.bias != bias || g_ec.slope != slope) {
        [enc setDepthBias:bias * 16777216.0f slopeScale:slope clamp:0];   // D3D bias is in D24 depth units (ADR-005)
        g_ec.bias = bias; g_ec.slope = slope;
    }
    uint32_t bf = rs[RS_BLENDFACTOR];
    if (first || g_ec.bf != bf) {
        [enc setBlendColorRed:((bf >> 16) & 0xff) / 255.0f green:((bf >> 8) & 0xff) / 255.0f blue:(bf & 0xff) / 255.0f alpha:(bf >> 24) / 255.0f];
        g_ec.bf = bf;
    }
    set_viewport_scissor(enc);
    if (g_active_q != g_enc_q) {
        [enc setVisibilityResultMode:g_active_q >= 0 ? MTLVisibilityResultModeCounting : MTLVisibilityResultModeDisabled
                              offset:g_active_q >= 0 ? (NSUInteger)g_active_q * 8 : 0];
        g_enc_q = g_active_q;
    }

    // ---- vertex stage
    tf2mt_vs_driver drv;
    memset(&drv, 0, sizeof drv);
    // D3D9 samples pixel centres at integer coordinates, Metal at +0.5: move geometry +0.5 px right and down
    drv.half_pixel[0] = S.vp[2] ? 1.0f / (float)S.vp[2] : 0.0f;
    drv.half_pixel[1] = S.vp[3] ? -1.0f / (float)S.vp[3] : 0.0f;
    memcpy(drv.clip_plane0, S.clip[0], 16);
    for (uint32_t s = 0; s < TF2MT_MAX_STREAMS; s++) {
        Obj *b = lookup(S.stream[s].h);
        id<MTLBuffer> buf = b && b->kind == K_BUFFER ? b->buf : g_dummybuf;
        drv.stream_words[s] = (uint32_t)([buf length] / 4);
        if (g_ec.vb[s] != buf) { [enc setVertexBuffer:buf offset:0 atIndex:3 + s]; g_ec.vb[s] = buf; }
    }
    Obj *dobj = lookup(S.decl);
    const msl::Reflection &vr = vs.out.refl;
    for (uint32_t v = 0; v < TF2MT_MAX_VS_INPUTS; v++) {
        drv.fetch[v].stream_type = 17u << 8;
        if (!(vr.vs_input_mask & (1u << v)) || !dobj || !dobj->decl) continue;
        for (const DeclElem &e : dobj->decl->e)
            if (e.usage == vr.vs_input_usage[v] && e.index == vr.vs_input_usage_index[v] && e.stream < TF2MT_MAX_STREAMS) {
                drv.fetch[v].stream_type = e.stream | (uint32_t)e.type << 8;
                drv.fetch[v].offset = S.stream[e.stream].off + e.offset;
                drv.fetch[v].stride = S.stream[e.stream].stride;
                break;
            }
    }
    if (g_vsf_dirty) { [enc setVertexBytes:S.vsf length:sizeof S.vsf atIndex:0]; g_vsf_dirty = false; }
    if (g_vsi_dirty) { [enc setVertexBytes:&S.vsi length:sizeof S.vsi atIndex:1]; g_vsi_dirty = false; }
    if (first || memcmp(&drv, &g_ec.vdrv, sizeof drv)) { [enc setVertexBytes:&drv length:sizeof drv atIndex:2]; g_ec.vdrv = drv; }

    // ---- fragment stage
    tf2mt_ps_driver pdrv;
    memset(&pdrv, 0, sizeof pdrv);
    pdrv.alpha_ref = (rs[RS_ALPHAREF] & 0xff) / 255.0f;
    if (!g_no_lodbias) for (uint32_t s = 0; s < 16; s++) memcpy(&pdrv.lod_bias[s], &S.ss[s][SS_MIPMAPLODBIAS], 4);
    if (g_psf_dirty) { [enc setFragmentBytes:S.psf length:sizeof S.psf atIndex:0]; g_psf_dirty = false; }
    if (g_psi_dirty) { [enc setFragmentBytes:&S.psi length:sizeof S.psi atIndex:1]; g_psi_dirty = false; }
    // Always sent (80 bytes). Skipping identical re-sends measurably changed output (m6-demo SSIM 0.9991 -> 0.9969
    // with byte-identical data); cause unexplained — see docs/m9-report.md. The other caches tested neutral.
    if (first || memcmp(&pdrv, &g_ec.pdrv, sizeof pdrv)) { [enc setFragmentBytes:&pdrv length:sizeof pdrv atIndex:2]; g_ec.pdrv = pdrv; }
    for (uint32_t s = 0; s < 16 && ps; s++) {
        const msl::Reflection &pr = ps->out.refl;
        if (!(pr.sampler_mask & (1u << s))) continue;
        uint8_t want = pr.sampler_type[s];
        id<MTLTexture> t = nil;
        Obj *to = lookup(S.tex[s]);
        if (to && to->kind == K_TEXTURE && to->tex && !to->fmt->depth) {
            uint32_t have = to->type == TF2MT_TEX_CUBE ? sm::TT_CUBE : to->type == TF2MT_TEX_3D ? sm::TT_VOLUME : sm::TT_2D;
            if (have == want) t = sampling_view(*to, S.ss[s][SS_SRGBTEXTURE] != 0);
            else log_once("textype", "texture type mismatch on stage %u (shader %u, bound %u): dummy bound", s, want, have);
        }
        if (!t) t = want == sm::TT_CUBE ? g_dummycube : want == sm::TT_VOLUME ? g_dummy3d : g_dummy2d;
        if (g_ec.tex[s] != t) { [enc setFragmentTexture:t atIndex:s]; g_ec.tex[s] = t; }
        id<MTLSamplerState> smp = sampler_for(S.ss[s]);
        if (g_ec.smp[s] != smp) { [enc setFragmentSamplerState:smp atIndex:s]; g_ec.smp[s] = smp; }
    }

    if (indexed) {
        Obj *ib = lookup(S.ib);
        if (!ib || ib->kind != K_BUFFER) { g_skipped++; log_once("noib", "draw skipped: no index buffer"); return; }
        bool i32 = S.ibfmt == 102;
        [enc drawIndexedPrimitives:mt indexCount:n indexType:i32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:ib->buf
                 indexBufferOffset:p[4] * (i32 ? 4u : 2u) instanceCount:1 baseVertex:(int32_t)p[1] baseInstance:0];
    } else {
        [enc drawPrimitives:mt vertexStart:p[1] vertexCount:n];
    }
    g_draws++;
}

// ---------------------------------------------------------------- clears
float ndc_x(int32_t x) { return 2.0f * (float)x / (float)g_rt_w - 1.0f; }
float ndc_y(int32_t y) { return 1.0f - 2.0f * (float)y / (float)g_rt_h; }

void clear(const uint32_t *p)
{
    uint32_t flags = p[0], color = p[1], st = p[3], nrect = p[4];
    float z; memcpy(&z, &p[2], 4);
    Attach a;
    if (!attachments_from_state(a)) return;
    if (!a.color) flags &= ~CLEAR_TARGET;
    if (!a.depth) flags &= ~(CLEAR_ZBUFFER | CLEAR_STENCIL);
    if (a.depth && !has_stencil(a.depth.pixelFormat)) flags &= ~CLEAR_STENCIL;
    if (!flags) return;
    float rgba[4] = {((color >> 16) & 0xff) / 255.0f, ((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f, (color >> 24) / 255.0f};
    // size of the target (needed to decide whether the clear covers it)
    uint32_t w = a.color ? std::max<uint32_t>(1, (uint32_t)a.color.width >> a.clevel) : (uint32_t)a.depth.width;
    uint32_t h = a.color ? std::max<uint32_t>(1, (uint32_t)a.color.height >> a.clevel) : (uint32_t)a.depth.height;
    // D3D9 clears the viewport rectangle (intersected with the scissor and the rects)
    int32_t l = (int32_t)S.vp[0], t = (int32_t)S.vp[1], r = (int32_t)(S.vp[0] + S.vp[2]), b = (int32_t)(S.vp[1] + S.vp[3]);
    if (S.rs[RS_SCISSORTESTENABLE]) { l = std::max(l, S.scissor[0]); t = std::max(t, S.scissor[1]); r = std::min(r, S.scissor[2]); b = std::min(b, S.scissor[3]); }
    l = std::max(l, 0); t = std::max(t, 0); r = std::min(r, (int32_t)w); b = std::min(b, (int32_t)h);
    bool full = nrect == 0 && l == 0 && t == 0 && r == (int32_t)w && b == (int32_t)h;
    if (full) {
        // a pass that will start with this clear: drop the open encoder for these attachments and clear on load
        end_encoder();
        if (flags & CLEAR_TARGET) { g_pending.color = true; memcpy(g_pending.rgba, rgba, 16); }
        if (flags & CLEAR_ZBUFFER) { g_pending.depth = true; g_pending.z = z; }
        if (flags & CLEAR_STENCIL) { g_pending.stencil = true; g_pending.st = st; }
        // keep color/depth loads of attachments that are not cleared
        encoder_for(a);
        return;
    }
    id<MTLRenderCommandEncoder> enc = encoder_for(a);
    MTLPixelFormat cf = a.color ? a.color.pixelFormat : MTLPixelFormatInvalid, df = a.depth ? a.depth.pixelFormat : MTLPixelFormatInvalid;
    id<MTLRenderPipelineState> pso = internal_pso(0, cf, df, has_stencil(df) ? df : MTLPixelFormatInvalid, samples_of(a), flags & CLEAR_TARGET);
    if (!pso) return;
    DssKey dk = {};
    dk.z = 8 | ((flags & CLEAR_ZBUFFER) ? 0x10 : 0);
    if (flags & CLEAR_STENCIL) { dk.s = 1; dk.front = dk.back = 1 | 1 << 4 | 3 << 8 | 8 << 12; dk.masks = 0xff | 0xff << 8; }
    [enc setRenderPipelineState:pso];
    if (g_enc_q != -1) { [enc setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0]; g_enc_q = -1; }
    [enc setDepthStencilState:dss_make(dk)];
    [enc setStencilReferenceValue:st & 0xff];
    [enc setCullMode:MTLCullModeNone];
    [enc setTriangleFillMode:MTLTriangleFillModeFill];
    [enc setDepthBias:0 slopeScale:0 clamp:0];
    MTLViewport v = {0, 0, (double)w, (double)h, 0, 1};
    [enc setViewport:v];
    struct { float rect[4]; float color[4]; float z; float pad[3]; } args;
    memcpy(args.color, rgba, 16);
    args.z = z;
    const int32_t *rects = (const int32_t *)(p + 5);
    uint32_t count = nrect ? nrect : 1;
    for (uint32_t i = 0; i < count; i++) {
        int32_t rl = l, rt = t, rr = r, rb = b;
        if (nrect) { rl = std::max(rl, rects[i * 4]); rt = std::max(rt, rects[i * 4 + 1]); rr = std::min(rr, rects[i * 4 + 2]); rb = std::min(rb, rects[i * 4 + 3]); }
        if (rr <= rl || rb <= rt) continue;
        MTLScissorRect sr = {(NSUInteger)rl, (NSUInteger)rt, (NSUInteger)(rr - rl), (NSUInteger)(rb - rt)};
        [enc setScissorRect:sr];
        args.rect[0] = ndc_x(rl); args.rect[1] = ndc_y(rt); args.rect[2] = ndc_x(rr); args.rect[3] = ndc_y(rb);
        [enc setVertexBytes:&args length:sizeof args atIndex:0];
        [enc setFragmentBytes:&args length:sizeof args atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    enc_cache_reset();   // the quad changed pipeline, depth state, viewport, scissor and buffer 0
}

// ---------------------------------------------------------------- copies
struct Surf { Obj *o = nullptr; uint32_t face = 0, level = 0, w = 0, h = 0; };
bool surf_of(uint32_t h, uint32_t face, uint32_t level, Surf &s)
{
    Obj *o = lookup(h);
    if (!o || o->kind != K_TEXTURE || !o->tex) return false;
    s.o = o; s.face = face; s.level = level;
    s.w = std::max(1u, o->width >> level); s.h = std::max(1u, o->height >> level);
    return true;
}

// MSAA surface -> single-sample temporary of the same size (render pass resolve)
// resolve targets are reused per (format, size): the MSAA back buffer resolves every frame
std::unordered_map<uint64_t, id<MTLTexture>> g_resolve;
id<MTLTexture> resolve_temp(const Surf &s)
{
    uint64_t key = (uint64_t)s.o->tex.pixelFormat << 40 | (uint64_t)s.w << 20 | s.h;
    id<MTLTexture> __strong &t = g_resolve[key];
    if (!t) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s.o->tex.pixelFormat width:s.w height:s.h mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        t = [tf2mt_metal_device() newTextureWithDescriptor:d];
    }
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = s.o->tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    rp.colorAttachments[0].resolveTexture = t;
    [[frame_cb_locked() renderCommandEncoderWithDescriptor:rp] endEncoding];
    return t;
}

// Draw `src` (2D, single-sample) scaled into dst's rect with the given filter.
void copy_quad(id<MTLTexture> src, const int32_t *sr, id<MTLTexture> dst, uint32_t dlevel, uint32_t dslice, const int32_t *dr,
               bool linear, uint32_t dw, uint32_t dh)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].level = dlevel;
    rp.colorAttachments[0].slice = dslice;
    bool whole = dr[0] == 0 && dr[1] == 0 && dr[2] == (int32_t)dw && dr[3] == (int32_t)dh;
    rp.colorAttachments[0].loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [frame_cb_locked() renderCommandEncoderWithDescriptor:rp];
    id<MTLRenderPipelineState> pso = internal_pso(1, dst.pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid, (uint32_t)dst.sampleCount, 1);
    if (pso) {
        float sw = (float)src.width, sh = (float)src.height;
        struct { float rect[4]; float uv[4]; } args = {
            {2.0f * dr[0] / dw - 1.0f, 1.0f - 2.0f * dr[1] / dh, 2.0f * dr[2] / dw - 1.0f, 1.0f - 2.0f * dr[3] / dh},
            {sr[0] / sw, sr[1] / sh, sr[2] / sw, sr[3] / sh}};
        [enc setRenderPipelineState:pso];
        MTLViewport v = {0, 0, (double)dw, (double)dh, 0, 1};
        [enc setViewport:v];
        [enc setVertexBytes:&args length:sizeof args atIndex:0];
        [enc setFragmentTexture:src atIndex:0];
        [enc setFragmentSamplerState:linear ? g_linear : g_point atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [enc endEncoding];
}

// 2D single-sample view of one subresource for sampling (cube faces / levels)
id<MTLTexture> subresource_view(const Surf &s)
{
    if (s.o->type == TF2MT_TEX_2D && s.level == 0 && s.o->tex.mipmapLevelCount == 1) return s.o->tex;
    return [s.o->tex newTextureViewWithPixelFormat:s.o->tex.pixelFormat textureType:MTLTextureType2D
                                            levels:NSMakeRange(s.level, 1) slices:NSMakeRange(s.face, 1)];
}

void stretch(const uint32_t *p)
{
    Surf src, dst;
    if (!surf_of(p[0], p[1], p[2], src) || !surf_of(p[8], p[9], p[10], dst)) { log_once("stretch", "StretchRect with unknown surfaces skipped"); return; }
    if (src.o->fmt->depth || dst.o->fmt->depth) { log_once("stretchds", "depth StretchRect not implemented"); return; }
    int32_t sr[4] = {0, 0, (int32_t)src.w, (int32_t)src.h}, dr[4] = {0, 0, (int32_t)dst.w, (int32_t)dst.h};
    if (p[3]) memcpy(sr, p + 4, 16);
    if (p[11]) memcpy(dr, p + 12, 16);
    end_encoder();
    id<MTLTexture> s = src.o->samples > 1 ? resolve_temp(src) : subresource_view(src);
    bool same_size = sr[2] - sr[0] == dr[2] - dr[0] && sr[3] - sr[1] == dr[3] - dr[1];
    if (same_size && dst.o->samples == 1 && s.pixelFormat == dst.o->tex.pixelFormat) {
        id<MTLBlitCommandEncoder> b = [frame_cb_locked() blitCommandEncoder];
        [b copyFromTexture:s sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(sr[0], sr[1], 0)
                sourceSize:MTLSizeMake(sr[2] - sr[0], sr[3] - sr[1], 1) toTexture:dst.o->tex destinationSlice:dst.face
          destinationLevel:dst.level destinationOrigin:MTLOriginMake(dr[0], dr[1], 0)];
        [b endEncoding];
        return;
    }
    copy_quad(s, sr, dst.o->tex, dst.level, dst.face, dr, p[16] != 1 /* D3DTEXF_POINT */, dst.w, dst.h);
}

void color_fill(const uint32_t *p)
{
    Surf s;
    if (!surf_of(p[0], p[1], p[2], s)) return;
    end_encoder();
    uint32_t c = p[8];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = s.o->tex;
    rp.colorAttachments[0].level = s.level; rp.colorAttachments[0].slice = s.face;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(((c >> 16) & 0xff) / 255.0, ((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0, (c >> 24) / 255.0);
    rp.colorAttachments[0].loadAction = p[3] ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> enc = [frame_cb_locked() renderCommandEncoderWithDescriptor:rp];
    if (p[3]) log_once("colorfillrect", "ColorFill with a rect: not implemented (whole surface untouched)");
    [enc endEncoding];
}

// ---------------------------------------------------------------- present
uint64_t g_frames;
double g_encode_ms;
void present(uint32_t bb, std::unique_lock<std::mutex> &lk)
{
    end_encoder();
    FrameTimes &ft = g_ft[(g_frames_total + 1) & 255];
    ft.enc = ms_now();
    g_ft[(g_frames_total + 2) & 255].cam = false;   // next frame's camera not seen yet
    CAMetalLayer *layer = tf2mt_metal_layer();
    id<CAMetalDrawable> drawable = nil;
    Surf s;
    bool drain = false;
    if (g_drain_on && g_drain_request.exchange(false) && layer && layer.displaySyncEnabled && ft.enc - g_last_drain_ms > 2000.0) {
        drain = true; g_last_drain_ms = ft.enc; g_drains++;
        // a real gap of one refresh with nothing submitted is what lets the compositor queue run empty (a quick next
        // frame refills it): skip this present and hold the encoder (and so the game) for one refresh
        lk.unlock(); usleep((useconds_t)(g_refresh_ms * 1000.0)); lk.lock();
        tf2mt_log("drain: skipped presenting one frame (%s, refresh %.2f ms, drains %llu)\n",
                  g_drains == 1 ? "probe" : "queue stuck", g_refresh_ms, (unsigned long long)g_drains);
    }
    if (!drain && layer && surf_of(bb, 0, 0, s)) {
        // waiting for a drawable (vsync) must not block the game thread's unix calls: drop the lock meanwhile and
        // look the back buffer up again afterwards (the object table may have grown)
        double w0 = ms_now();
        lk.unlock();
        drawable = tf2mt_next_drawable();
        ft.drawable = ms_now();
        lk.lock();
        g_encode_ms -= ms_now() - w0;
        if (drawable && surf_of(bb, 0, 0, s)) {
            id<MTLTexture> src = s.o->samples > 1 ? resolve_temp(s) : s.o->tex;
            id<MTLTexture> dt = drawable.texture;
            int32_t sr[4] = {0, 0, (int32_t)s.w, (int32_t)s.h}, dr[4] = {0, 0, (int32_t)dt.width, (int32_t)dt.height};
            copy_quad(src, sr, dt, 0, 0, dr, true, (uint32_t)dt.width, (uint32_t)dt.height);
        }
    }
    if (drawable) {
        uint64_t k = g_frames_total + 1;
        double start = (k >= 2) ? g_frame_start_ms[k & 255] : 0.0;
        FrameTimes t = g_ft[k & 255];   // commit time is filled in below, after this copy: pass it separately
        double *commit_slot = &g_ft[k & 255].commit;
        [drawable addPresentedHandler:^(id<MTLDrawable> d) {
            double shown = d.presentedTime * 1000.0;
            std::lock_guard<std::mutex> g(g_lat_mu);
            if (shown <= 0.0) { g_lat_dropped++; return; }
            {   // queue-drain bookkeeping
                double commit = *commit_slot, ts = shown - commit;
                if (g_last_shown_ms > 0.0) { double iv = shown - g_last_shown_ms; if (iv > 3.0 && iv < 25.0) g_refresh_ms += (iv - g_refresh_ms) * 0.02; }
                if (commit > 0.0 && ts > 0.0 && ts < 200.0) g_sec_ts.push_back(ts);
                if (g_sec_start == 0.0) g_sec_start = g_first_shown = shown;
                if (!g_probe_done && shown - g_first_shown > 15000.0) { g_probe_done = true; g_drain_request = true; }
                if (shown - g_sec_start >= 500.0) {   // half a second of shown frames
                    if (g_sec_ts.size() >= 12) {
                        std::nth_element(g_sec_ts.begin(), g_sec_ts.begin() + g_sec_ts.size() / 2, g_sec_ts.end());
                        double med = g_sec_ts[g_sec_ts.size() / 2];
                        g_sec_medians.push_back(med);
                        if (g_sec_medians.size() > 600) g_sec_medians.pop_front();
                        g_ts_floor = *std::min_element(g_sec_medians.begin(), g_sec_medians.end());
                        if (med > g_ts_floor + 0.5 * g_refresh_ms) { if (++g_late_secs >= 2) { g_drain_request = true; g_late_secs = 0; } }
                        else g_late_secs = 0;
                    }
                    g_sec_ts.clear(); g_sec_start = shown;
                }
            }
            if (FILE *f = latency_csv(); f && start > 0.0) {
                double commit = *commit_slot;
                fprintf(f, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f\n", (unsigned long long)k, start,
                        t.submit - start, t.drawable - t.enc, commit - t.drawable, shown - commit, shown - start,
                        t.cam ? t.yaw : -999.0f, t.cam ? t.pitch : -999.0f);
            }
            if (g_last_shown_ms > 0.0) { g_shown_n++; if (shown - g_last_shown_ms > 12.5) g_shown_jumps++; }
            g_last_shown_ms = shown;
            if (start > 0.0 && shown > start) g_lat_samples.push_back((float)(shown - start));
        }];
    }
    submit_locked(false, drawable);
    ft.commit = ms_now();
    {
        double w0 = ms_now();
        lk.unlock();
        wait_inflight_unlocked();
        lk.lock();
        g_encode_ms -= ms_now() - w0;
    }
    tf2mt_present_tick();
    g_frames_total++;
    g_stall_ms_frame = 0;
    if ((g_frames + 1) % 600 == 0) {
        uint64_t n = g_gpu_n.exchange(0);
        double sum = g_gpu_ms_sum.exchange(0), mx = g_gpu_ms_max.exchange(0);
        tf2mt_log("ledger-perf: last 600 frames: backend encode %.3f ms/frame, GPU %.3f ms/frame (max %.2f), %llu submissions\n",
                  g_encode_ms / 600.0, n ? sum / n : 0.0, mx, (unsigned long long)n);
        g_encode_ms = 0;
        std::vector<float> lat; uint64_t dropped, shown_n, jumps;
        { std::lock_guard<std::mutex> g(g_lat_mu); lat.swap(g_lat_samples); dropped = g_lat_dropped; g_lat_dropped = 0;
          shown_n = g_shown_n; jumps = g_shown_jumps; g_shown_n = g_shown_jumps = 0; }
        if (!lat.empty()) {
            std::sort(lat.begin(), lat.end());
            auto q = [&](double f) { return lat[std::min(lat.size() - 1, (size_t)(f * lat.size()))]; };
            tf2mt_log("latency: frame start -> on screen, last %zu frames: p50 %.1f p90 %.1f p99 %.1f max %.1f ms; "
                      "%llu drawables not shown; shown intervals >12.5 ms: %llu of %llu; pacing ahead=%d drawables=%d inflight=%s\n",
                      lat.size(), q(.5), q(.9), q(.99), (double)lat.back(), (unsigned long long)dropped, (unsigned long long)jumps, (unsigned long long)shown_n,
                      g_game_ahead, (int)tf2mt_metal_layer().maximumDrawableCount, getenv("TF2MT_INFLIGHT") ? getenv("TF2MT_INFLIGHT") : getenv("TF2MT_MAX_LATENCY") ? getenv("TF2MT_MAX_LATENCY") : "2");
        }
    }
    if (++g_frames % 600 == 0)
        tf2mt_log("render: %llu frames, %llu draws, %llu skipped; caches: %zu PSOs, %zu DSS, %zu samplers; "
                  "pso: %llu sync misses, %llu prewarmed, %llu adopted, %llu draws deferred (pipeline compiling); %llu stalls\n",
                  (unsigned long long)g_frames, (unsigned long long)g_draws, (unsigned long long)g_skipped,
                  g_psos.size(), g_dss.size(), g_samplers.size(), (unsigned long long)g_pso_misses,
                  (unsigned long long)g_prewarmed, (unsigned long long)g_adopted, (unsigned long long)g_skipped_translucent,
                  (unsigned long long)g_stall_events);
}

} // namespace

// ---------------------------------------------------------------- backend.h hooks
namespace tf2mt::be {
void end_frame_encoders_locked() { end_encoder(); }
// A destroyed shader's address can be reused by the next allocation: its cached pipelines must go with it.
void shader_destroyed_locked(Shader *sh)
{
    for (auto it = g_psos.begin(); it != g_psos.end();)
        it = (it->first.vs == sh || it->first.ps == sh) ? g_psos.erase(it) : std::next(it);
    for (auto it = g_building.begin(); it != g_building.end();)
        it = (it->first.vs == sh || it->first.ps == sh) ? g_building.erase(it) : std::next(it);
}
}

// ---------------------------------------------------------------- unix calls
extern "C" NTSTATUS unix_create_shader(void *args)
{
    auto *p = (tf2mt_shader_params *)args;
    std::lock_guard<std::mutex> g(g_mu);
    init_internal();
    std::string err;
    auto sh = make_shader((const uint32_t *)(uintptr_t)p->code, p->dwords, err);
    if (sh) {
        sh->hash = fnv((const void *)(uintptr_t)p->code, (size_t)p->dwords * 4) ^ 0x7466326d74000001ull;   // translator v1
        g_live[sh->hash] = sh;
        prewarm_for(sh);
    }
    uint32_t h = alloc_handle();
    if (!h) { p->status = 2; return STATUS_SUCCESS; }
    Obj &o = obj_at(h);
    o.kind = K_SHADER;
    o.shader = sh;
    p->handle = h;
    p->status = sh ? 0 : 1;
    if (!sh) tf2mt_log("shader rejected: %s\n", err.c_str());
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS unix_create_decl(void *args)
{
    auto *p = (tf2mt_decl_params *)args;
    std::lock_guard<std::mutex> g(g_mu);
    auto d = std::make_shared<VertexDecl>();
    const uint8_t *e = (const uint8_t *)(uintptr_t)p->elements;
    for (uint32_t i = 0; i < p->count; i++, e += 8) {
        uint16_t stream; memcpy(&stream, e, 2);
        if (stream == 0xff) break;
        DeclElem x; x.stream = stream; memcpy(&x.offset, e + 2, 2); x.type = e[4]; x.method = e[5]; x.usage = e[6]; x.index = e[7];
        d->e.push_back(x);
    }
    uint32_t h = alloc_handle();
    Obj &o = obj_at(h);
    o.kind = K_DECL;
    o.decl = d;
    p->handle = h;
    return STATUS_SUCCESS;
}


// ---------------------------------------------------------------- encoder thread (M9, PLAN §8.2)
// The game thread's SUBMIT copies the batch into this queue and returns; one unix-side thread decodes and encodes
// Metal work in order. The game may run at most one presented frame ahead of the encoder (back-pressure in
// SUBMIT). Synchronous waits remain only where D3D9 requires results: readback and GetData(FLUSH) (drain_queue).
namespace {
std::mutex g_qmu;
std::condition_variable g_qcv;
std::deque<std::vector<uint32_t>> g_queue;
uint64_t g_enq_batches, g_done_batches;
int g_pending_presents;
bool g_encoder_started;

void decode_batch(const uint32_t *w, const uint32_t *end, std::unique_lock<std::mutex> &lk)
{
    unsigned since_yield = 0;
    while (w < end) {
        if (++since_yield == 256) { since_yield = 0; lk.unlock(); lk.lock(); }   // let game-thread unix calls in
        uint32_t op = *w & 0xff, n = *w >> 8;
        const uint32_t *p = w + 1;
        w = p + n;
        if (w > end) { tf2mt_log("command stream overrun\n"); break; }
        switch (op) {
        case TF2MT_CMD_RESET_STATE: reset_state(); enc_cache_reset(); break;
        case TF2MT_CMD_RS: if (p[0] < 256) { if (p[0] == RS_SRGBWRITEENABLE && S.rs[p[0]] != p[1]) {} S.rs[p[0]] = p[1]; } break;
        case TF2MT_CMD_SS: if (p[0] < NSTAGE && p[1] < SS_N) S.ss[p[0]][p[1]] = p[2]; break;
        case TF2MT_CMD_TEXTURE: if (p[0] < NSTAGE) S.tex[p[0]] = p[1]; break;
        case TF2MT_CMD_STREAM: if (p[0] < 16) { S.stream[p[0]].h = p[1]; S.stream[p[0]].off = p[2]; S.stream[p[0]].stride = p[3]; } break;
        case TF2MT_CMD_INDICES: S.ib = p[0]; S.ibfmt = p[1]; break;
        case TF2MT_CMD_VDECL: S.decl = p[0]; break;
        case TF2MT_CMD_VS: S.vs = p[0]; break;
        case TF2MT_CMD_PS: S.ps = p[0]; break;
        case TF2MT_CMD_VS_F:
            if (p[0] + p[1] <= 256) {
                memcpy(S.vsf[p[0]], p + 2, p[1] * 16); g_vsf_dirty = true;
                // camera smoothness (latency CSV): first upload of c8..c11 per frame = view-projection; c11 (the
                // clip-space w row) is the camera forward vector -> yaw/pitch
                FrameTimes &cf = g_ft[(g_frames_total + 1) & 255];
                if (!cf.cam && p[0] <= 8 && p[0] + p[1] >= 12) {
                    const float *wr = (const float *)(p + 2) + (11 - p[0]) * 4;
                    if (wr[0] != 0.f || wr[1] != 0.f) {
                        cf.yaw = atan2f(wr[1], wr[0]) * 57.29578f;
                        cf.pitch = atan2f(wr[2], sqrtf(wr[0] * wr[0] + wr[1] * wr[1])) * 57.29578f;
                        cf.cam = true;
                    }
                }
            }
            break;
        case TF2MT_CMD_PS_F: if (p[0] + p[1] <= 224) { memcpy(S.psf[p[0]], p + 2, p[1] * 16); g_psf_dirty = true; } break;
        case TF2MT_CMD_VS_I: if (p[0] + p[1] <= 16) { memcpy(S.vsi.i[p[0]], p + 2, p[1] * 16); g_vsi_dirty = true; } break;
        case TF2MT_CMD_PS_I: if (p[0] + p[1] <= 16) { memcpy(S.psi.i[p[0]], p + 2, p[1] * 16); g_psi_dirty = true; } break;
        case TF2MT_CMD_VS_B: g_vsi_dirty = true; for (uint32_t i = 0; i < p[1] && p[0] + i < 16; i++) S.vsi.b = (S.vsi.b & ~(1u << (p[0] + i))) | (p[2 + i] ? 1u : 0u) << (p[0] + i); break;
        case TF2MT_CMD_PS_B: g_psi_dirty = true; for (uint32_t i = 0; i < p[1] && p[0] + i < 16; i++) S.psi.b = (S.psi.b & ~(1u << (p[0] + i))) | (p[2 + i] ? 1u : 0u) << (p[0] + i); break;
        case TF2MT_CMD_RT: if (p[0] < 4) { S.rt[p[0]] = {p[1], p[2], p[3]}; } break;
        case TF2MT_CMD_DS: S.ds = p[0]; break;
        case TF2MT_CMD_VIEWPORT: memcpy(S.vp, p, 16); memcpy(S.vpz, p + 4, 8); break;
        case TF2MT_CMD_SCISSOR: memcpy(S.scissor, p, 16); break;
        case TF2MT_CMD_CLIP_PLANE: if (p[0] < 6) memcpy(S.clip[p[0]], p + 1, 16); break;
        case TF2MT_CMD_CLEAR: clear(p); break;
        case TF2MT_CMD_DRAW_INDEXED: draw(true, p); break;
        case TF2MT_CMD_DRAW: draw(false, p); break;
        case TF2MT_CMD_STRETCH: stretch(p); break;
        case TF2MT_CMD_COLOR_FILL: color_fill(p); break;
        case TF2MT_CMD_PRESENT: present(p[0], lk); break;
        case TF2MT_CMD_QUERY_BEGIN:
            if (p[0] < TF2MT_QUERY_SLOTS) {
                ((uint64_t *)[g_vis contents])[p[0]] = 0;
                __atomic_store_n(&g_qserial[p[0]], UINT64_MAX, __ATOMIC_RELEASE); __atomic_store_n(&g_qgen[p[0]], p[1], __ATOMIC_RELEASE);
                g_active_q = (int32_t)p[0];
            }
            break;
        case TF2MT_CMD_QUERY_END:
            if (p[0] < TF2MT_QUERY_SLOTS) {
                __atomic_store_n(&g_qserial[p[0]], g_submitted + 1, __ATOMIC_RELEASE); __atomic_store_n(&g_qgen[p[0]], p[1], __ATOMIC_RELEASE);
                if (g_active_q == (int32_t)p[0]) g_active_q = -1;
            }
            break;
        case TF2MT_CMD_EVENT:
            if (p[0] < TF2MT_QUERY_SLOTS) { __atomic_store_n(&g_qserial[p[0]], g_submitted + 1, __ATOMIC_RELEASE); __atomic_store_n(&g_qgen[p[0]], p[1], __ATOMIC_RELEASE); }
            break;
        case TF2MT_CMD_DESTROY: destroy_locked(p[0]); break;
        case TF2MT_CMD_BUFFER_SWITCH: buffer_switch_locked(p[0], p[1]); break;
        default: log_once("badcmd", "unknown command %u", op); break;
        }
    }
}

int count_presents(const uint32_t *w, const uint32_t *end)
{
    int n = 0;
    while (w < end) { if ((*w & 0xff) == TF2MT_CMD_PRESENT) n++; w += 1 + (*w >> 8); }
    return n;
}

void encoder_loop()
{
    for (;;) {
        std::vector<uint32_t> batch;
        {
            std::unique_lock<std::mutex> q(g_qmu);
            g_qcv.wait(q, [] { return !g_queue.empty(); });
            batch = std::move(g_queue.front());
            g_queue.pop_front();
        }
        int presents = count_presents(batch.data(), batch.data() + batch.size());
        @autoreleasepool {
            std::unique_lock<std::mutex> lk(g_mu);
            double t0 = ms_now();
            decode_batch(batch.data(), batch.data() + batch.size(), lk);
            if (g_flush_requested.exchange(false)) submit_locked(false, nil);   // a GetData(FLUSH) is waiting on it
            g_encode_ms += ms_now() - t0;
        }
        {
            std::lock_guard<std::mutex> q(g_qmu);
            g_done_batches++;
            g_pending_presents -= presents;
        }
        g_qcv.notify_all();
    }
}

} // namespace

namespace tf2mt::be {
void drain_queue()
{
    std::unique_lock<std::mutex> q(g_qmu);
    uint64_t target = g_enq_batches;
    g_qcv.wait(q, [&] { return g_done_batches >= target; });
}
} // namespace tf2mt::be

namespace {
void start_encoder_locked()
{
    if (g_encoder_started) return;
    g_encoder_started = true;
    std::thread(encoder_loop).detach();
}
} // namespace

extern "C" NTSTATUS unix_query_data(void *args)
{
    // Lock-free: the game polls occlusion queries ~15x per frame and must not wait behind the encoder (which holds
    // g_mu while it encodes). Slot generation / serial are written by the encoder; completion by Metal handlers.
    auto *q = (tf2mt_query_params *)args;
    q->done = 0; q->value = 0;
    if (q->slot >= TF2MT_QUERY_SLOTS || !g_vis) return STATUS_SUCCESS;
    uint32_t gen = __atomic_load_n(&g_qgen[q->slot], __ATOMIC_ACQUIRE);
    uint64_t serial = __atomic_load_n(&g_qserial[q->slot], __ATOMIC_ACQUIRE);
    if (gen == q->gen && serial != UINT64_MAX && g_completed.load() >= serial) {
        q->done = 1;
        q->value = __atomic_load_n(&((uint64_t *)[g_vis contents])[q->slot], __ATOMIC_RELAXED);
        return STATUS_SUCCESS;
    }
    // D3DGETDATA_FLUSH: make sure the work gets submitted, without waiting for it. An empty batch wakes the
    // encoder even when the game sends nothing else until the query completes (frame-latency limiter loops).
    if (q->flush && (gen != q->gen || serial == UINT64_MAX || serial > __atomic_load_n(&g_submitted, __ATOMIC_ACQUIRE))
        && !g_flush_requested.exchange(true)) {
        std::lock_guard<std::mutex> l(g_qmu);
        g_queue.emplace_back();
        g_enq_batches++;
        g_qcv.notify_all();
    }
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS unix_submit(void *args)
{
    auto *sp = (tf2mt_submit_params *)args;
    {
        std::lock_guard<std::mutex> g(g_mu);
        init_internal();
        start_encoder_locked();
    }
    const uint32_t *w = (const uint32_t *)(uintptr_t)sp->data, *end = w + sp->bytes / 4;
    int presents = count_presents(w, end);
    if (presents) g_ft[(g_sub_presents.load() + 1) & 255].submit = ms_now();   // game finished this frame
    {
        std::unique_lock<std::mutex> q(g_qmu);
        g_queue.emplace_back(w, end);
        g_enq_batches++;
        g_pending_presents += presents;
        g_qcv.notify_all();
        // back-pressure: the game may be at most g_game_ahead presented frames ahead of the encoder
        // (TF2MT_GAME_AHEAD, default 0 = lowest latency; 1 halves spike repeats but the owner felt the extra frame as drag; docs/mouse-input.md)
        if (presents) g_qcv.wait(q, [] { return g_pending_presents <= g_game_ahead; });
    }
    if (presents) {
        uint64_t k = g_sub_presents.fetch_add((uint64_t)presents) + (uint64_t)presents;   // frame k was just submitted
        g_frame_start_ms[(k + 1) & 255] = ms_now();   // frame k+1 starts now
    }
    return STATUS_SUCCESS;
}
