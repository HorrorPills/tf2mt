// tf2mt backend internals shared by resources.mm (M5) and render.mm (M6). Unix side only (ObjC++).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include "../common/unix_calls.h"

typedef int32_t NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)

extern "C" id<MTLDevice> tf2mt_metal_device(void);
extern "C" id<MTLCommandQueue> tf2mt_metal_queue(void);
extern "C" void tf2mt_log(const char *fmt, ...);

namespace tf2mt::be {

struct Fmt {
    uint32_t d3d;
    MTLPixelFormat pf;
    uint8_t block_dim, block_bytes;   // 1 x bpp for uncompressed
    bool depth, srgb_view;
    MTLTextureSwizzleChannels swz;    // D3D sampling semantics, applied through a view when bound
    const char *note;
};
const Fmt *find_fmt(uint32_t d3d);
bool swizzled(const Fmt *f);

struct Shader;       // render.mm
struct VertexDecl;   // render.mm

enum Kind : uint8_t { K_FREE, K_BUFFER, K_TEXTURE, K_SHADER, K_DECL };
struct Retired { id<MTLBuffer> buf; uint64_t serial; };
struct Obj {
    uint32_t gen = 0;
    Kind kind = K_FREE;
    // buffer
    id<MTLBuffer> buf = nil;
    uint32_t size = 0;
    bool dynamic = false;
    std::vector<Retired> retired;
    std::vector<std::pair<uint32_t, id<MTLBuffer>>> pending;   // renamed backings not yet switched in (token, buffer)
    uint32_t rename_seq = 0;
    // texture
    id<MTLTexture> tex = nil;
    const Fmt *fmt = nullptr;
    uint32_t type = 0, width = 0, height = 0, depth = 0, levels = 0, samples = 1, usage = 0;
    uint64_t bytes = 0;
    id<MTLTexture> view_linear = nil, view_srgb = nil;   // sampling views (swizzle / sRGB), created on first use
    std::vector<id<MTLTexture>> rt_views;                 // render-target views per (face, level, srgb)
    // shader / declaration
    std::shared_ptr<Shader> shader;
    std::shared_ptr<VertexDecl> decl;
};

extern std::mutex g_mu;                // guards everything below; every unix call takes it
extern tf2mt_stats g_stats;
uint32_t alloc_handle();
Obj *lookup(uint32_t h);
Obj &obj_at(uint32_t h);               // after alloc_handle()
void free_handle(uint32_t h);

// Submission: the upload command buffer (blits) always commits before the frame command buffer (render work);
// both get the same serial. Buffer backings retired during a frame are recyclable once that serial completes.
extern std::atomic<uint64_t> g_completed;
extern uint64_t g_submitted;
extern std::atomic<double> g_gpu_ms_sum, g_gpu_ms_max;   // GPU time of committed frame work (ledger)
extern std::atomic<uint64_t> g_gpu_n;
id<MTLCommandBuffer> frame_cb_locked();          // render work of the current frame (created on demand)
void end_frame_encoders_locked();                // render.mm: close any open encoder before commit
void submit_locked(bool wait, id<CAMetalDrawable> present);
void wait_inflight_unlocked();                   // frame-latency limit (2 in flight); call WITHOUT g_mu
void drain_queue();                              // render.mm: wait until the encoder thread has processed everything
void destroy_locked(uint32_t h);                 // resources.mm
void buffer_switch_locked(uint32_t h, uint32_t token);
void shader_destroyed_locked(Shader *sh);         // render.mm: drop cached pipelines that use it

} // namespace tf2mt::be
