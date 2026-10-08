// tf2mt unix library (x86_64 Mach-O, loaded by Wine next to the builtin tf2mt.dll). M2 spike: Metal device,
// CAMetalLayer on the game window (via this Wine build's exported `macdrv_functions` table, see ADR-002),
// clear-and-present, and present-interval statistics.
//
// Wine unix-call ABI (Wine's public builtin-DLL design): the PE side obtains a handle with
// NtQueryVirtualMemory(..., MemoryWineUnixFuncs) and calls __wine_unix_call_dispatcher(handle, code, args);
// Wine then calls __wine_unix_call_funcs[code](args) here.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../common/unix_calls.h"

typedef int32_t NTSTATUS;
#define STATUS_SUCCESS 0
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001)

// ---- this Wine build's macdrv interface (layout documented in ADR-002) ----
typedef void *macdrv_view, *macdrv_window, *macdrv_metal_device, *macdrv_metal_view, *macdrv_metal_layer;
struct macdrv_win_data { void *hwnd; macdrv_window cocoa_window; macdrv_view cocoa_view; macdrv_view client_cocoa_view; };
struct macdrv_functions_t {
    void (*init_display_devices)(int);
    struct macdrv_win_data *(*get_win_data)(void *hwnd);
    void (*release_win_data)(struct macdrv_win_data *data);
    macdrv_window (*get_cocoa_window)(void *hwnd, int require_on_screen);
    macdrv_metal_device (*create_metal_device)(void);
    void (*release_metal_device)(macdrv_metal_device d);
    macdrv_metal_view (*view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
    macdrv_metal_layer (*view_get_metal_layer)(macdrv_metal_view v);
    void (*view_release_metal_view)(macdrv_metal_view v);
    void (*on_main_thread)(dispatch_block_t b);
};

static id<MTLDevice> g_device;
static id<MTLCommandQueue> g_queue;
static CAMetalLayer *g_layer;
static macdrv_metal_view g_metal_view;
static struct macdrv_functions_t *g_macdrv;
static FILE *g_log;

// present-interval statistics (mach ticks)
static mach_timebase_info_data_t g_tb;
static uint64_t g_last, g_frames;
static double g_sum, g_sumsq, g_max;

static void tlog(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}

// shared with resources.mm
id<MTLDevice> tf2mt_metal_device(void) { return g_device; }
id<MTLCommandQueue> tf2mt_metal_queue(void) { return g_queue; }
void tf2mt_log(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fflush(g_log);
}
void tf2mt_resources_init(void);
CAMetalLayer *tf2mt_metal_layer(void) { return g_layer; }
static void present_tick(void);
void tf2mt_present_tick(void) { present_tick(); }
NTSTATUS unix_create_buffer(void *), unix_rename_buffer(void *), unix_create_texture(void *), unix_destroy(void *),
         unix_upload(void *), unix_readback(void *), unix_flush(void *), unix_stats(void *),
         unix_create_shader(void *), unix_create_decl(void *), unix_submit(void *), unix_query_data(void *);

static NTSTATUS unix_init(void *args)
{
    struct tf2mt_init_params *p = args;
    if (p->log_path[0]) g_log = fopen(p->log_path, "a");
    mach_timebase_info(&g_tb);
    g_device = MTLCreateSystemDefaultDevice();
    if (!g_device) { tlog("no Metal device\n"); return STATUS_UNSUCCESSFUL; }
    g_queue = [g_device newCommandQueue];
    g_macdrv = dlsym(RTLD_DEFAULT, "macdrv_functions");
    tf2mt_resources_init();
    tlog("tf2mt.so init: device '%s', macdrv_functions %p\n", g_device.name.UTF8String, (void *)g_macdrv);
    return g_macdrv ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
}

static NTSTATUS unix_attach(void *args)
{
    struct tf2mt_attach_params *p = args;
    void *hwnd = (void *)(uintptr_t)p->hwnd;
    struct macdrv_win_data *wd = g_macdrv->get_win_data(hwnd);
    if (!wd) { tlog("attach: no win data for hwnd %#llx\n", (unsigned long long)p->hwnd); return STATUS_UNSUCCESSFUL; }
    macdrv_view view = wd->client_cocoa_view;
    g_macdrv->release_win_data(wd);
    if (!view) { tlog("attach: no client view\n"); return STATUS_UNSUCCESSFUL; }
    g_metal_view = g_macdrv->view_create_metal_view(view, (__bridge macdrv_metal_device)g_device);
    g_layer = (__bridge CAMetalLayer *)g_macdrv->view_get_metal_layer(g_metal_view);
    if (!g_layer) { tlog("attach: no metal layer\n"); return STATUS_UNSUCCESSFUL; }
    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = YES;
    g_layer.opaque = YES;
    // 2 drawables (+ game ahead 0, render.mm): bench demo, vsync 120 Hz, frame start -> on screen 25 ms vs 41 ms with
    // 3 drawables + game ahead 1 (docs/mouse-input.md). Earlier online test (2 drawables + 1 in flight, game ahead 1,
    // m_filter 1) showed more missed refreshes (405/min vs 216/min): watch jumps online. TF2MT_DRAWABLES=3 restores.
    g_layer.maximumDrawableCount = 2;
    const char *nd = getenv("TF2MT_DRAWABLES");   // explicit override (2 or 3) for latency experiments
    if (nd && (atoi(nd) == 2 || atoi(nd) == 3)) g_layer.maximumDrawableCount = atoi(nd);
    g_layer.displaySyncEnabled = p->vsync ? YES : NO;
    g_layer.drawableSize = CGSizeMake(p->width, p->height);
    tlog("attach: hwnd %#llx view %p layer %p %ux%u vsync %u\n", (unsigned long long)p->hwnd, view,
         (__bridge void *)g_layer, p->width, p->height, p->vsync);
    return STATUS_SUCCESS;
}

static NTSTATUS unix_present(void *args)
{
    struct tf2mt_present_params *p = args;
    if (!g_layer) return STATUS_UNSUCCESSFUL;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = [g_layer nextDrawable];
        if (!drawable) return STATUS_UNSUCCESSFUL;
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(p->r, p->g, p->b, 1.0);
        id<MTLCommandBuffer> cb = [g_queue commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }
    present_tick();
    return STATUS_SUCCESS;
}

static void present_tick(void)
{
    uint64_t now = mach_absolute_time();
    if (g_last) {
        double ms = (double)(now - g_last) * g_tb.numer / g_tb.denom / 1e6;
        g_sum += ms; g_sumsq += ms * ms; if (ms > g_max) g_max = ms;
        if (++g_frames % 600 == 0) {
            double mean = g_sum / 600, sd = sqrt(g_sumsq / 600 - mean * mean);
            tlog("present: %llu frames, last 600: mean %.3f ms (%.1f fps) sd %.3f max %.2f\n",
                 (unsigned long long)g_frames, mean, 1000.0 / mean, sd, g_max);
            g_sum = g_sumsq = g_max = 0;
        }
    }
    g_last = now;
}

static NTSTATUS unix_query_device(void *args)
{
    struct tf2mt_device_info *p = args;
    if (!g_device) return STATUS_UNSUCCESSFUL;
    memset(p, 0, sizeof *p);
    strlcpy(p->name, g_device.name.UTF8String, sizeof p->name);
    p->recommended_working_set = g_device.recommendedMaxWorkingSetSize;
    for (uint32_t n = 1; n <= 8; n <<= 1)
        if ([g_device supportsTextureSampleCount:n]) { p->msaa_mask |= n; p->max_msaa = n; }
    return STATUS_SUCCESS;
}

static NTSTATUS unix_detach(void *args)
{
    if (g_metal_view) g_macdrv->view_release_metal_view(g_metal_view);
    g_metal_view = NULL; g_layer = nil;
    tlog("detach\n");
    return STATUS_SUCCESS;
}

// Every unix call runs inside an autorelease pool: Wine's threads have no run loop, so autoreleased Metal objects
// (command buffers, encoders, descriptors) would otherwise live forever (M7 soak: ~128 MB per map change).
static NTSTATUS pooled_unix_init(void *a) { @autoreleasepool { return unix_init(a); } }
static NTSTATUS pooled_unix_attach(void *a) { @autoreleasepool { return unix_attach(a); } }
static NTSTATUS pooled_unix_present(void *a) { @autoreleasepool { return unix_present(a); } }
static NTSTATUS pooled_unix_detach(void *a) { @autoreleasepool { return unix_detach(a); } }
static NTSTATUS pooled_unix_query_device(void *a) { @autoreleasepool { return unix_query_device(a); } }
static NTSTATUS pooled_unix_create_buffer(void *a) { @autoreleasepool { return unix_create_buffer(a); } }
static NTSTATUS pooled_unix_rename_buffer(void *a) { @autoreleasepool { return unix_rename_buffer(a); } }
static NTSTATUS pooled_unix_create_texture(void *a) { @autoreleasepool { return unix_create_texture(a); } }
static NTSTATUS pooled_unix_destroy(void *a) { @autoreleasepool { return unix_destroy(a); } }
static NTSTATUS pooled_unix_upload(void *a) { @autoreleasepool { return unix_upload(a); } }
static NTSTATUS pooled_unix_readback(void *a) { @autoreleasepool { return unix_readback(a); } }
static NTSTATUS pooled_unix_flush(void *a) { @autoreleasepool { return unix_flush(a); } }
static NTSTATUS pooled_unix_stats(void *a) { @autoreleasepool { return unix_stats(a); } }
static NTSTATUS pooled_unix_create_shader(void *a) { @autoreleasepool { return unix_create_shader(a); } }
static NTSTATUS pooled_unix_create_decl(void *a) { @autoreleasepool { return unix_create_decl(a); } }
static NTSTATUS pooled_unix_submit(void *a) { @autoreleasepool { return unix_submit(a); } }
static NTSTATUS pooled_unix_query_data(void *a) { @autoreleasepool { return unix_query_data(a); } }

__attribute__((visibility("default")))
const void *__wine_unix_call_funcs[] = {
    [TF2MT_UNIX_INIT] = pooled_unix_init,
    [TF2MT_UNIX_ATTACH] = pooled_unix_attach,
    [TF2MT_UNIX_PRESENT] = pooled_unix_present,
    [TF2MT_UNIX_DETACH] = pooled_unix_detach,
    [TF2MT_UNIX_QUERY_DEVICE] = pooled_unix_query_device,
    [TF2MT_UNIX_CREATE_BUFFER] = pooled_unix_create_buffer,
    [TF2MT_UNIX_RENAME_BUFFER] = pooled_unix_rename_buffer,
    [TF2MT_UNIX_CREATE_TEXTURE] = pooled_unix_create_texture,
    [TF2MT_UNIX_DESTROY] = pooled_unix_destroy,
    [TF2MT_UNIX_UPLOAD] = pooled_unix_upload,
    [TF2MT_UNIX_READBACK] = pooled_unix_readback,
    [TF2MT_UNIX_FLUSH] = pooled_unix_flush,
    [TF2MT_UNIX_STATS] = pooled_unix_stats,
    [TF2MT_UNIX_CREATE_SHADER] = pooled_unix_create_shader,
    [TF2MT_UNIX_CREATE_DECL] = pooled_unix_create_decl,
    [TF2MT_UNIX_SUBMIT] = pooled_unix_submit,
    [TF2MT_UNIX_QUERY_DATA] = pooled_unix_query_data,
};
_Static_assert(TF2MT_UNIX_QUERY_DATA + 1 == TF2MT_UNIX_COUNT, "unix call table");
