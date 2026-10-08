// tf2mt backend resources (M5, PLAN §8.1/§8.3): Metal buffers and textures behind the frontend's D3D9 objects.
//
// * Handles: 32-bit generational ids (index bits 0-19, generation bits 20-31) into one table.
// * Buffers: MTLStorageModeShared. The frontend writes straight into `contents` (same process, unified memory):
//   Lock = pointer arithmetic, no unix call. Dynamic buffers are renamed on DISCARD (RENAME_BUFFER): the old backing
//   is retired with the current submission serial and recycled once the GPU has completed that serial.
// * Textures: MTLStorageModePrivate (lossless compression on Apple GPUs). Uploads are packed into a shared staging
//   ring and copied with a blit in submission order. Pending blits are committed on FLUSH (Present) or when a
//   staging chunk fills.
// * TF2MT_VERIFY_UPLOADS=1: every upload is read back and compared byte for byte with its source (M5 acceptance).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>
#include "backend.h"

namespace tf2mt::be {

// ---------------------------------------------------------------- formats (census + every format the adapter claims)
constexpr MTLTextureSwizzleChannels SW(MTLTextureSwizzle r, MTLTextureSwizzle g, MTLTextureSwizzle b, MTLTextureSwizzle a) { return {r, g, b, a}; }
#define ID SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha)
#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
const Fmt kFormats[] = {
    {21, MTLPixelFormatBGRA8Unorm, 1, 4, false, true, ID, "A8R8G8B8"},
    {22, MTLPixelFormatBGRA8Unorm, 1, 4, false, true, SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne), "X8R8G8B8"},
    {23, MTLPixelFormatB5G6R5Unorm, 1, 2, false, false, ID, "R5G6B5 (channel order unverified)"},
    {24, MTLPixelFormatBGR5A1Unorm, 1, 2, false, false, SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleOne), "X1R5G5B5 (unverified)"},
    {25, MTLPixelFormatBGR5A1Unorm, 1, 2, false, false, ID, "A1R5G5B5 (unverified)"},
    {26, MTLPixelFormatABGR4Unorm, 1, 2, false, false, ID, "A4R4G4B4 (unverified)"},
    {28, MTLPixelFormatA8Unorm, 1, 1, false, false, ID, "A8"},
    {36, MTLPixelFormatRGBA16Unorm, 1, 8, false, false, ID, "A16B16G16R16"},
    {50, MTLPixelFormatR8Unorm, 1, 1, false, false, SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne), "L8"},
    {51, MTLPixelFormatRG8Unorm, 1, 2, false, false, SW(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen), "A8L8"},
    {60, MTLPixelFormatRG8Snorm, 1, 2, false, false, SW(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleOne, MTLTextureSwizzleOne), "V8U8"},
    {62, MTLPixelFormatRGBA8Snorm, 1, 4, false, false, ID, "X8L8V8U8 (approximate: L treated as signed)"},
    {63, MTLPixelFormatRGBA8Snorm, 1, 4, false, false, ID, "Q8W8V8U8"},
    {75, MTLPixelFormatDepth32Float_Stencil8, 1, 4, true, false, ID, "D24S8"},
    {77, MTLPixelFormatDepth32Float_Stencil8, 1, 4, true, false, ID, "D24X8"},
    {80, MTLPixelFormatDepth16Unorm, 1, 2, true, false, ID, "D16"},
    {113, MTLPixelFormatRGBA16Float, 1, 8, false, false, ID, "A16B16G16R16F"},
    {114, MTLPixelFormatR32Float, 1, 4, false, false, SW(MTLTextureSwizzleRed, MTLTextureSwizzleOne, MTLTextureSwizzleOne, MTLTextureSwizzleOne), "R32F"},
    {116, MTLPixelFormatRGBA32Float, 1, 16, false, false, ID, "A32B32G32R32F"},
    {FOURCC('D', 'X', 'T', '1'), MTLPixelFormatBC1_RGBA, 4, 8, false, true, ID, "DXT1"},
    {FOURCC('D', 'X', 'T', '3'), MTLPixelFormatBC2_RGBA, 4, 16, false, true, ID, "DXT3"},
    {FOURCC('D', 'X', 'T', '5'), MTLPixelFormatBC3_RGBA, 4, 16, false, true, ID, "DXT5"},
    {FOURCC('A', 'T', 'I', '1'), MTLPixelFormatBC4_RUnorm, 4, 8, false, false, ID, "ATI1N"},
    {FOURCC('A', 'T', 'I', '2'), MTLPixelFormatBC5_RGUnorm, 4, 16, false, false, ID, "ATI2N"},
    {FOURCC('I', 'N', 'T', 'Z'), MTLPixelFormatDepth32Float_Stencil8, 1, 4, true, false, ID, "INTZ"},
    {FOURCC('D', 'F', '2', '4'), MTLPixelFormatDepth32Float, 1, 4, true, false, ID, "DF24"},
    {FOURCC('D', 'F', '1', '6'), MTLPixelFormatDepth16Unorm, 1, 2, true, false, ID, "DF16"},
    {FOURCC('N', 'U', 'L', 'L'), MTLPixelFormatInvalid, 1, 0, false, false, ID, "NULL (no storage)"},
};

const Fmt *find_fmt(uint32_t d3d)
{
    for (const Fmt &f : kFormats) if (f.d3d == d3d) return &f;
    return nullptr;
}

bool swizzled(const Fmt *f)
{
    return f->swz.red != MTLTextureSwizzleRed || f->swz.green != MTLTextureSwizzleGreen || f->swz.blue != MTLTextureSwizzleBlue
        || f->swz.alpha != MTLTextureSwizzleAlpha;
}

// ---------------------------------------------------------------- handle table
std::mutex g_mu;
static std::vector<Obj> g_objs(1);    // index 0 unused
static std::vector<uint32_t> g_free;
tf2mt_stats g_stats;

uint32_t alloc_handle()
{
    uint32_t idx;
    if (!g_free.empty()) { idx = g_free.back(); g_free.pop_back(); }
    else { idx = (uint32_t)g_objs.size(); g_objs.emplace_back(); }
    if (idx >= (1u << 20)) return 0;
    Obj &o = g_objs[idx];
    o.gen = (o.gen + 1) & 0xfff;
    if (!o.gen) o.gen = 1;
    return idx | o.gen << 20;
}
Obj *lookup(uint32_t h)
{
    uint32_t idx = h & 0xfffff;
    if (!h || idx >= g_objs.size()) return nullptr;
    Obj &o = g_objs[idx];
    return (o.kind != K_FREE && o.gen == h >> 20) ? &o : nullptr;
}
Obj &obj_at(uint32_t h) { return g_objs[h & 0xfffff]; }
void free_handle(uint32_t h)
{
    Obj &o = g_objs[h & 0xfffff];
    uint32_t gen = o.gen;
    o = Obj();
    o.gen = gen;
    g_free.push_back(h & 0xfffff);
}

std::atomic<uint64_t> g_completed{0};
uint64_t g_submitted = 0;
std::atomic<double> g_gpu_ms_sum{0}, g_gpu_ms_max{0};
std::atomic<uint64_t> g_gpu_n{0};

} // namespace tf2mt::be

namespace {
using namespace tf2mt::be;
bool g_verify;

// ---------------------------------------------------------------- staging ring for uploads
struct Chunk { id<MTLBuffer> buf; size_t used = 0; uint64_t serial = 0; };
constexpr size_t kChunk = 32u << 20;
std::deque<Chunk> g_chunks;           // front = oldest
Chunk *g_cur;
id<MTLCommandBuffer> g_cb;            // uploads
id<MTLBlitCommandEncoder> g_blit;
id<MTLCommandBuffer> g_frame_cb;      // render work
std::deque<id<MTLCommandBuffer>> g_inflight;   // presented frames not yet known complete
} // namespace

namespace tf2mt::be {
id<MTLCommandBuffer> frame_cb_locked()
{
    if (!g_frame_cb) { g_frame_cb = [tf2mt_metal_queue() commandBuffer]; g_frame_cb.label = @"tf2mt frame"; }
    return g_frame_cb;
}

// Commit uploads, then render work, under one serial (queue order guarantees uploads execute first).
void submit_locked(bool wait, id<CAMetalDrawable> present)
{
    end_frame_encoders_locked();
    uint64_t serial = ++g_submitted;
    for (Chunk &c : g_chunks) if (c.serial == 0 && c.used) c.serial = serial;
    if (g_cb) { [g_blit endEncoding]; [g_cb commit]; }
    id<MTLCommandBuffer> fcb = frame_cb_locked();
    if (present) [fcb presentDrawable:present];
    [fcb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        double gpu = (cb.GPUEndTime - cb.GPUStartTime) * 1000.0;   // ledger (PLAN §11): GPU ms per submission
        if (gpu > 0) { g_gpu_ms_sum += gpu; g_gpu_n++; double m = g_gpu_ms_max.load(); while (gpu > m && !g_gpu_ms_max.compare_exchange_weak(m, gpu)) {} }
        uint64_t prev = g_completed.load();
        while (prev < serial && !g_completed.compare_exchange_weak(prev, serial)) {}
    }];
    [fcb commit];
    if (wait) [fcb waitUntilCompleted];
    if (present) g_inflight.push_back(fcb);
    g_cb = nil; g_blit = nil; g_cur = nullptr; g_frame_cb = nil;
}

// Frames in flight: TF2MT_MAX_LATENCY (default 2, PLAN D6; 1 tested online and missed more refreshes). Called by the encoder thread without g_mu, so uploads and other unix calls
// from the game thread are never blocked behind the GPU.
void wait_inflight_unlocked()
{
    for (;;) {
        id<MTLCommandBuffer> oldest = nil;
        {
            std::lock_guard<std::mutex> g(g_mu);
            static size_t limit = [] { const char *v = getenv("TF2MT_INFLIGHT"); if (!v) v = getenv("TF2MT_MAX_LATENCY"); int n = v ? atoi(v) : 2; return (size_t)(n < 1 ? 1 : n > 3 ? 3 : n); }();
            if (g_inflight.size() <= limit) return;   // frames in flight (default 1: lowest input latency)
            oldest = g_inflight.front();
            g_inflight.pop_front();
        }
        [oldest waitUntilCompleted];
    }
}
} // namespace tf2mt::be

namespace {
void commit_locked() { submit_locked(false, nil); }

id<MTLBlitCommandEncoder> blit_locked()
{
    if (!g_cb) {
        g_cb = [tf2mt_metal_queue() commandBuffer];
        g_cb.label = @"tf2mt uploads";
        g_blit = [g_cb blitCommandEncoder];
    }
    return g_blit;
}

// Reserve `n` bytes (256-aligned) of staging memory for the current upload command buffer.
uint8_t *stage_locked(size_t n, id<MTLBuffer> *buf, size_t *off)
{
    n = (n + 255) & ~size_t(255);
    if (g_cur && g_cur->used + n > [g_cur->buf length]) { commit_locked(); }
    if (!g_cur) {
        // recycle the oldest chunk whose command buffer has completed
        uint64_t done = g_completed.load();
        for (auto it = g_chunks.begin(); it != g_chunks.end(); ++it)
            if (it->serial && it->serial <= done && [it->buf length] >= n) {
                Chunk c = *it; g_chunks.erase(it);
                c.used = 0; c.serial = 0;
                g_chunks.push_back(c);
                g_cur = &g_chunks.back();
                break;
            }
        if (!g_cur) {
            Chunk c;
            c.buf = [tf2mt_metal_device() newBufferWithLength:std::max(kChunk, n) options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            if (!c.buf) return nullptr;
            g_chunks.push_back(c);
            g_cur = &g_chunks.back();
        }
    }
    *buf = g_cur->buf;
    *off = g_cur->used;
    uint8_t *p = (uint8_t *)[g_cur->buf contents] + g_cur->used;
    g_cur->used += n;
    return p;
}

// Geometry of a subresource region in the formats' blocks.
struct Region { uint32_t row_bytes, rows, w, h; };
Region region_of(const Obj &o, uint32_t level, uint32_t w, uint32_t h)
{
    uint32_t lw = std::max(1u, o.width >> level), lh = std::max(1u, o.height >> level);
    w = std::min(w, lw); h = std::min(h, lh);
    uint32_t bd = o.fmt->block_dim;
    return {(w + bd - 1) / bd * o.fmt->block_bytes, (h + bd - 1) / bd, w, h};
}

uint64_t now_ns() { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return mach_absolute_time() * tb.numer / tb.denom; }

bool readback_locked(Obj &o, const tf2mt_upload_params &p, uint8_t *dst, uint32_t dst_pitch, uint32_t dst_slice);

} // namespace

// ---------------------------------------------------------------- unix calls
extern "C" NTSTATUS unix_create_buffer(void *args)
{
    auto *p = (tf2mt_buffer_params *)args;
    std::lock_guard<std::mutex> g(g_mu);
    id<MTLBuffer> b = [tf2mt_metal_device() newBufferWithLength:std::max(p->size, 16u) options:MTLResourceStorageModeShared];
    if (!b) { p->status = 2; return STATUS_SUCCESS; }
    uint32_t h = alloc_handle();
    if (!h) { p->status = 2; return STATUS_SUCCESS; }
    Obj &o = g_objs[h & 0xfffff];
    o.kind = K_BUFFER; o.buf = b; o.size = p->size; o.dynamic = p->dynamic != 0; o.retired.clear();
    p->handle = h; p->cpu = (uint64_t)(uintptr_t)[b contents]; p->status = 0;
    g_stats.buffers_live++; g_stats.buffer_bytes_live += p->size;
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS unix_rename_buffer(void *args)
{
    auto *p = (tf2mt_buffer_params *)args;
    std::lock_guard<std::mutex> g(g_mu);
    Obj *o = lookup(p->handle);
    if (!o || o->kind != K_BUFFER) { p->status = 1; return STATUS_SUCCESS; }
    g_stats.renames++;
    // Hand out a free backing now (the game writes into it immediately); it becomes current only when the encoder
    // reaches TF2MT_CMD_BUFFER_SWITCH, which also retires the old one under the serial of the frame using it.
    uint64_t done = g_completed.load();
    id<MTLBuffer> next = nil;
    for (size_t i = 0; i < o->retired.size(); i++)
        if (o->retired[i].serial <= done) { next = o->retired[i].buf; o->retired.erase(o->retired.begin() + i); break; }
    if (!next) {
        next = [tf2mt_metal_device() newBufferWithLength:std::max(o->size, 16u) options:MTLResourceStorageModeShared];
        g_stats.rename_allocs++;
        if (!next) { p->status = 2; return STATUS_SUCCESS; }
    }
    p->token = ++o->rename_seq;
    o->pending.push_back({p->token, next});
    p->cpu = (uint64_t)(uintptr_t)[next contents];
    p->status = 0;
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS unix_create_texture(void *args)
{
    auto *p = (tf2mt_texture_params *)args;
    std::lock_guard<std::mutex> g(g_mu);
    const Fmt *f = find_fmt(p->d3dfmt);
    if (!f) { p->status = 1; tf2mt_log("create_texture: unsupported D3DFORMAT 0x%x\n", p->d3dfmt); return STATUS_SUCCESS; }
    uint32_t h = alloc_handle();
    if (!h) { p->status = 2; return STATUS_SUCCESS; }
    Obj &o = g_objs[h & 0xfffff];
    o.kind = K_TEXTURE; o.fmt = f; o.usage = p->usage; o.type = p->type; o.width = p->width; o.height = p->height;
    o.depth = std::max(1u, p->depth); o.levels = std::max(1u, p->levels); o.samples = std::max(1u, p->samples);
    o.tex = nil; o.bytes = 0;
    if (f->pf != MTLPixelFormatInvalid) {      // D3DFMT_NULL: render-target placeholder without storage
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.pixelFormat = f->pf;
        d.width = p->width; d.height = p->height;
        d.mipmapLevelCount = o.levels;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        if (p->usage & 0x3) d.usage |= MTLTextureUsageRenderTarget;          // D3DUSAGE_RENDERTARGET | DEPTHSTENCIL
        if (f->srgb_view || swizzled(f)) d.usage |= MTLTextureUsagePixelFormatView;   // sRGB / swizzled sampling views
        if (p->type == TF2MT_TEX_CUBE) d.textureType = MTLTextureTypeCube;
        else if (p->type == TF2MT_TEX_3D) { d.textureType = MTLTextureType3D; d.depth = o.depth; }
        else if (o.samples > 1) { d.textureType = MTLTextureType2DMultisample; d.sampleCount = o.samples; d.mipmapLevelCount = 1; }
        else d.textureType = MTLTextureType2D;
        o.tex = [tf2mt_metal_device() newTextureWithDescriptor:d];
        if (!o.tex) {
            free_handle(h);
            p->status = 2;
            tf2mt_log("create_texture: allocation failed (%s %ux%ux%u lv %u)\n", f->note, p->width, p->height, o.depth, o.levels);
            return STATUS_SUCCESS;
        }
        o.bytes = [o.tex allocatedSize];
    }
    p->handle = h; p->status = 0;
    g_stats.textures_live++; g_stats.texture_bytes_live += o.bytes;
    return STATUS_SUCCESS;
}

namespace tf2mt::be {
// stream-order switch to a renamed backing (encoder thread)
void buffer_switch_locked(uint32_t h, uint32_t token)
{
    Obj *o = lookup(h);
    if (!o || o->kind != K_BUFFER) return;
    for (size_t i = 0; i < o->pending.size(); i++)
        if (o->pending[i].first == token) {
            o->retired.push_back({o->buf, g_submitted + 1});   // the frame being encoded is the last user of the old one
            o->buf = o->pending[i].second;
            o->pending.erase(o->pending.begin() + i);
            while (o->retired.size() > 8) o->retired.erase(o->retired.begin());   // bound memory
            return;
        }
}
void destroy_locked(uint32_t h)
{
    Obj *o = lookup(h);
    if (!o) return;
    if (o->kind == K_BUFFER) { g_stats.buffers_live--; g_stats.buffer_bytes_live -= o->size; }
    else if (o->kind == K_TEXTURE) { g_stats.textures_live--; g_stats.texture_bytes_live -= o->bytes; }
    else if (o->kind == K_SHADER && o->shader) shader_destroyed_locked(o->shader.get());
    free_handle(h);   // Metal keeps resources referenced by committed command buffers alive
}
} // namespace tf2mt::be

extern "C" NTSTATUS unix_destroy(void *args)
{
    auto *p = (tf2mt_handle_params *)args;
    drain_queue();   // queued commands may still reference it (the frontend normally uses TF2MT_CMD_DESTROY)
    std::lock_guard<std::mutex> g(g_mu);
    Obj *o = lookup(p->handle);
    if (!o) return STATUS_SUCCESS;
    if (o->kind == K_BUFFER) { g_stats.buffers_live--; g_stats.buffer_bytes_live -= o->size; }
    else if (o->kind == K_TEXTURE) { g_stats.textures_live--; g_stats.texture_bytes_live -= o->bytes; }
    else if (o->kind == K_SHADER && o->shader) shader_destroyed_locked(o->shader.get());
    // Metal keeps resources referenced by committed command buffers alive; dropping our references is enough.
    free_handle(p->handle);
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS unix_upload(void *args)
{
    auto *p = (tf2mt_upload_params *)args;
    uint64_t t0 = now_ns();
    std::lock_guard<std::mutex> g(g_mu);
    Obj *o = lookup(p->handle);
    if (!o || o->kind != K_TEXTURE || !o->tex || o->fmt->depth || o->samples > 1) { p->status = 1; return STATUS_SUCCESS; }
    if (p->level >= o->levels) { p->status = 1; return STATUS_SUCCESS; }
    Region r = region_of(*o, p->level, p->width, p->height);
    uint32_t ld = o->type == TF2MT_TEX_3D ? std::max(1u, o->depth >> p->level) : 1;
    uint32_t depth = std::min(std::max(1u, p->depth), ld - std::min(p->z, ld - 1));
    size_t slice = (size_t)r.row_bytes * r.rows, total = slice * depth;
    id<MTLBuffer> sb; size_t off;
    uint8_t *dst = stage_locked(total, &sb, &off);
    if (!dst) { p->status = 2; return STATUS_SUCCESS; }
    const uint8_t *src = (const uint8_t *)(uintptr_t)p->src;
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < r.rows; y++)
            memcpy(dst + z * slice + (size_t)y * r.row_bytes, src + (size_t)z * p->slice_pitch + (size_t)y * p->row_pitch, r.row_bytes);
    [blit_locked() copyFromBuffer:sb sourceOffset:off sourceBytesPerRow:r.row_bytes sourceBytesPerImage:slice
                       sourceSize:MTLSizeMake(r.w, r.h, depth) toTexture:o->tex
                 destinationSlice:o->type == TF2MT_TEX_CUBE ? p->face : 0 destinationLevel:p->level
                destinationOrigin:MTLOriginMake(p->x, p->y, o->type == TF2MT_TEX_3D ? p->z : 0)];
    g_stats.upload_bytes += total; g_stats.upload_calls++;
    p->status = 0;
    if (g_verify) {
        std::vector<uint8_t> back(total);
        tf2mt_upload_params q = *p;
        q.width = r.w; q.height = r.h; q.depth = depth;
        bool ok = readback_locked(*o, q, back.data(), r.row_bytes, (uint32_t)slice);
        bool same = ok;
        for (uint32_t z = 0; z < depth && same; z++)
            for (uint32_t y = 0; y < r.rows && same; y++)
                same = !memcmp(back.data() + z * slice + (size_t)y * r.row_bytes, src + (size_t)z * p->slice_pitch + (size_t)y * p->row_pitch, r.row_bytes);
        if (same) g_stats.verify_ok++;
        else {
            if (g_stats.verify_fail++ < 20)
                tf2mt_log("VERIFY FAIL: %s level %u face %u region %u,%u %ux%ux%u\n", o->fmt->note, p->level, p->face, p->x, p->y, r.w, r.h, depth);
        }
    }
    g_stats.upload_ns += now_ns() - t0;
    return STATUS_SUCCESS;
}

namespace {
// Synchronous texture -> CPU copy of one region (flushes pending uploads first so it sees them).
bool readback_locked(Obj &o, const tf2mt_upload_params &p, uint8_t *dst, uint32_t dst_pitch, uint32_t dst_slice)
{
    if (!o.tex || o.fmt->depth || o.samples > 1) return false;
    Region r = region_of(o, p.level, p.width, p.height);
    uint32_t depth = std::max(1u, p.depth);
    size_t slice = (size_t)r.row_bytes * r.rows;
    commit_locked();
    id<MTLBuffer> rb = [tf2mt_metal_device() newBufferWithLength:slice * depth options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [tf2mt_metal_queue() commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:o.tex sourceSlice:o.type == TF2MT_TEX_CUBE ? p.face : 0 sourceLevel:p.level
          sourceOrigin:MTLOriginMake(p.x, p.y, o.type == TF2MT_TEX_3D ? p.z : 0) sourceSize:MTLSizeMake(r.w, r.h, depth)
              toBuffer:rb destinationOffset:0 destinationBytesPerRow:r.row_bytes destinationBytesPerImage:slice];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const uint8_t *s = (const uint8_t *)[rb contents];
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < r.rows; y++) memcpy(dst + (size_t)z * dst_slice + (size_t)y * dst_pitch, s + z * slice + (size_t)y * r.row_bytes, r.row_bytes);
    g_stats.readback_bytes += slice * depth;
    return true;
}
} // namespace

extern "C" NTSTATUS unix_readback(void *args)
{
    auto *p = (tf2mt_upload_params *)args;
    drain_queue();   // everything rendered before the readback must be encoded first
    std::lock_guard<std::mutex> g(g_mu);
    Obj *o = lookup(p->handle);
    p->status = (o && o->kind == K_TEXTURE && readback_locked(*o, *p, (uint8_t *)(uintptr_t)p->src, p->row_pitch, p->slice_pitch)) ? 0 : 1;
    return STATUS_SUCCESS;
}

// Frame boundary (Present): commit pending uploads; with nothing pending, commit an empty command buffer so the
// submission serial still advances and retired dynamic-buffer backings become recyclable.
extern "C" NTSTATUS unix_flush(void *)
{
    drain_queue();
    std::lock_guard<std::mutex> g(g_mu);
    blit_locked();
    commit_locked();
    return STATUS_SUCCESS;
}

extern "C" NTSTATUS unix_stats(void *args)
{
    std::lock_guard<std::mutex> g(g_mu);
    *(tf2mt_stats *)args = g_stats;
    return STATUS_SUCCESS;
}

extern "C" void tf2mt_resources_init(void)
{
    const char *v = getenv("TF2MT_VERIFY_UPLOADS");
    g_verify = v && *v == '1';
    if (g_verify) tf2mt_log("resources: upload verification ON\n");
}
