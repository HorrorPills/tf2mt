/* src/frontend — tf2mt.dll (x86_64 PE, Wine builtin) — M2 spike / M3 starting point.
 * Device = the Phase-0 null renderer (every call accepted, nothing drawn) plus presentation through the
 * tf2mt.so unix library: a CAMetalLayer on the game window, cleared to a pulsing colour each Present.
 * Origin: copied from tools/null/null.c (Phase 0); the comments below describe the null-device behaviour.
 *
 * - IDirect3D9(Ex) is tf2mt's own (adapter.c); it reproduces what the DXVK reference reports (census-derived tables),
 *   so adapter identity, caps and CheckDevice* answers — and therefore every rendering decision Source
 *   makes (dxlevel, HDR mode, shadow technique) — are identical to the DXVK baseline. Only CreateDevice
 *   is ours.
 * - The device and all resources are ours: refcounted objects, real (lazily committed) memory behind
 *   every Lock, immediate query results. Every vtable slot not implemented below is a loud stub that
 *   logs its name once and returns D3D_OK.
 * Not production code; the real frontend (src/frontend) is designed separately.
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include "methods.h"
#include "ifaces.h"
#include "../common/unix_calls.h"
#include "../common/commands.h"

/* ---- Wine unix-call ABI (Wine's public builtin-DLL design; declared here, not copied from Wine) ---- */
typedef UINT64 unixlib_handle_t;
__declspec(dllimport) extern NTSTATUS (WINAPI *__wine_unix_call_dispatcher)(unixlib_handle_t, unsigned int, void *);
NTSTATUS WINAPI NtQueryVirtualMemory(HANDLE, const void *, int, void *, SIZE_T, SIZE_T *);
#define MemoryWineUnixFuncs 1000
static unixlib_handle_t unix_handle;
/* per-frame attribution for the frame log: QPC ticks spent inside tf2mt.so, split into the Present flush
 * (includes waiting for a drawable / vsync) and everything else (creation, uploads, mid-frame flushes) */
static volatile LONG64 g_layer_ticks, g_present_ticks;
static volatile LONG g_in_present;
static NTSTATUS unix_call(enum tf2mt_unix_call code, void *args)
{
    if (!unix_handle) return (NTSTATUS)0xC0000001;
    LARGE_INTEGER a, b;
    QueryPerformanceCounter(&a);
    NTSTATUS st = __wine_unix_call_dispatcher(unix_handle, code, args);
    QueryPerformanceCounter(&b);
    InterlockedAdd64(g_in_present ? &g_present_ticks : &g_layer_ticks, b.QuadPart - a.QuadPart);
    return st;
}
NTSTATUS tf2mt_unix_call(int code, void *args) { return unix_call((enum tf2mt_unix_call)code, args); }
HRESULT tf2mt_adapter_caps(D3DCAPS9 *caps);
HRESULT tf2mt_adapter_display_mode(D3DDISPLAYMODE *m);
IDirect3D9Ex *tf2mt_adapter_create(void);
static int g_vsync = 1, g_attached;
static LARGE_INTEGER g_t0, g_freq;

/* ---------------------------------------------------------------- logging + stubs */
static HANDLE logf_ = INVALID_HANDLE_VALUE;
void logmsg(const char *fmt, ...)
{
    char b[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    DWORD w;
    if (logf_ != INVALID_HANDLE_VALUE && n > 0) WriteFile(logf_, b, (DWORD)(n < (int)sizeof b ? n : (int)sizeof b - 1), &w, NULL);
}

static volatile LONG unimpl_seen[NIFACES][256];
HRESULT null_unimpl(DWORD code)
{
    DWORD iface = code >> 16, m = code & 0xffff;
    if (!InterlockedExchange(&unimpl_seen[iface][m], 1))
        logmsg("UNIMPL %s::%s\n", iface_names[iface], iface_method_names[iface][m]);
    return D3D_OK;
}

static HRESULT WINAPI ok(void) { return D3D_OK; }


/* ---------------------------------------------------------------- formats */
static int is_bc8(D3DFORMAT f)  { return f == D3DFMT_DXT1 || f == (D3DFORMAT)MAKEFOURCC('A','T','I','1'); }
static int is_bc16(D3DFORMAT f) { return f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5
                                      || f == (D3DFORMAT)MAKEFOURCC('A','T','I','2'); }
static UINT bpp(D3DFORMAT f)
{
    switch ((DWORD)f) {
    case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4: return 1;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
    case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_L16: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D15S1: case D3DFMT_R16F: case D3DFMT_A8R3G3B2: case D3DFMT_CxV8U8: case MAKEFOURCC('D','F','1','6'): return 2;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    default: return 4;
    }
}
static UINT pitch_of(D3DFORMAT f, UINT w)
{
    if (is_bc8(f)) return ((w + 3) / 4) * 8;
    if (is_bc16(f)) return ((w + 3) / 4) * 16;
    return w * bpp(f);
}
static UINT rows_of(D3DFORMAT f, UINT h) { return (is_bc8(f) || is_bc16(f)) ? (h + 3) / 4 : h; }

/* ---------------------------------------------------------------- objects */
typedef struct obj obj_t;
struct obj {
    void **vtbl;
    volatile LONG ref;
    obj_t *container;         /* level surfaces/volumes: refcount lives in the container */
    struct device *dev;
    D3DRESOURCETYPE type;
    D3DFORMAT fmt; DWORD usage; D3DPOOL pool; D3DMULTISAMPLE_TYPE ms; DWORD fvf;
    UINT w, h, d, levels;     /* buffers: w = byte size */
    obj_t **subs; UINT nsubs; /* texture levels (cube: face*levels + level) */
    BYTE *volatile mem; SIZE_T memsize; UINT pitch, slice;
    void *blob; UINT blobsize; /* shader bytecode / declaration elements */
    D3DQUERYTYPE qtype;
    /* M5: backend (Metal) storage. Textures / standalone surfaces / buffers own a handle; level surfaces and
     * volumes address their container's handle with (face, level). */
    UINT32 gpu;               /* tf2mt.so handle, 0 = CPU-only object (SYSTEMMEM/SCRATCH) */
    UINT face, level;         /* level surfaces / volumes: subresource of the container */
    BYTE gpu_mem;             /* buffers: `mem` is the backend's shared MTLBuffer (never freed here) */
    BYTE upload_on_unlock;    /* a lock is open whose contents must reach the GPU copy at Unlock */
    RECT lrect; D3DBOX lbox;  /* the open lock's region */
};

struct device {
    void **vtbl;
    volatile LONG ref;
    IDirect3D9Ex *d3d;         /* wrapper object handed out by Direct3DCreate9(Ex) */
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DPRESENT_PARAMETERS pp;
    obj_t *backbuffer, *autods, *swapchain;
    obj_t *rt[4], *ds;
    obj_t *tex[20];
    DWORD rs[256];
    D3DVIEWPORT9 vp;
    RECT scissor;
};

static void *v_dev[M_IDirect3DDevice9Ex_N], *v_surf[M_IDirect3DSurface9_N], *v_tex[M_IDirect3DTexture9_N],
            *v_cube[M_IDirect3DCubeTexture9_N], *v_vol[M_IDirect3DVolumeTexture9_N], *v_volume[M_IDirect3DVolume9_N],
            *v_vb[M_IDirect3DVertexBuffer9_N], *v_ib[M_IDirect3DIndexBuffer9_N], *v_decl[M_IDirect3DVertexDeclaration9_N],
            *v_vs[M_IDirect3DVertexShader9_N], *v_ps[M_IDirect3DPixelShader9_N], *v_query[M_IDirect3DQuery9_N],
            *v_sb[M_IDirect3DStateBlock9_N], *v_swap[M_IDirect3DSwapChain9Ex_N];

static obj_t *new_obj(void **vtbl, struct device *dev, D3DRESOURCETYPE type)
{
    obj_t *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    o->vtbl = vtbl; o->ref = 1; o->dev = dev; o->type = type;
    return o;
}

/* ---------------------------------------------------------------- M6 command stream (src/common/commands.h)
 * Device calls append packets; the stream is decoded by the backend at Present and before every operation whose
 * effect must not overtake queued work (buffer renaming, readback, object destruction). One device per process. */
static CRITICAL_SECTION g_cs;
static UINT32 *g_cmd;
static size_t g_cmd_len;              /* dwords */
#define CMD_CAP (1u << 20)            /* 4 MB */
static void cmd_flush_locked(void)
{
    if (!g_cmd_len) return;
    struct tf2mt_submit_params p = {(UINT64)(ULONG_PTR)g_cmd, (UINT32)(g_cmd_len * 4), 0};
    unix_call(TF2MT_UNIX_SUBMIT, &p);
    g_cmd_len = 0;
}
static void cmd_flush(void)
{
    if (!g_cmd) return;
    EnterCriticalSection(&g_cs); cmd_flush_locked(); LeaveCriticalSection(&g_cs);
}
/* one packet: header + fixed dwords + optional variable part */
static void cmd(UINT32 op, const UINT32 *a, UINT32 na, const void *b, UINT32 nb)
{
    if (!g_cmd) return;
    EnterCriticalSection(&g_cs);
    if (g_cmd_len + 1 + na + nb > CMD_CAP) cmd_flush_locked();
    g_cmd[g_cmd_len++] = TF2MT_CMD_HEADER(op, na + nb);
    memcpy(g_cmd + g_cmd_len, a, na * 4); g_cmd_len += na;
    if (nb) { memcpy(g_cmd + g_cmd_len, b, nb * 4); g_cmd_len += nb; }
    LeaveCriticalSection(&g_cs);
}
#define CMD(op, ...) do { const UINT32 a_[] = {__VA_ARGS__}; cmd(op, a_, sizeof a_ / 4, NULL, 0); } while (0)

static void gpu_destroy(UINT32 h)
{
    /* in stream order: queued draws may still reference it (the encoder thread processes the stream later) */
    if (h) CMD(TF2MT_CMD_DESTROY, h);
}

static void free_obj(obj_t *o)
{
    for (UINT i = 0; i < o->nsubs; i++) free_obj(o->subs[i]);
    if (o->subs) HeapFree(GetProcessHeap(), 0, o->subs);
    if (o->gpu) gpu_destroy(o->gpu);
    if (o->mem && !o->gpu_mem) VirtualFree(o->mem, 0, MEM_RELEASE);
    if (o->blob) HeapFree(GetProcessHeap(), 0, o->blob);
    HeapFree(GetProcessHeap(), 0, o);
}

static ULONG WINAPI o_AddRef(obj_t *o)
{
    return o->container ? (ULONG)InterlockedIncrement(&o->container->ref) : (ULONG)InterlockedIncrement(&o->ref);
}
static ULONG WINAPI o_Release(obj_t *o)
{
    if (o->container) o = o->container;
    LONG r = InterlockedDecrement(&o->ref);
    if (!r) free_obj(o);
    return (ULONG)r;
}
static HRESULT WINAPI o_QueryInterface(obj_t *o, REFIID riid, void **out)
{
    o_AddRef(o); *out = o; /* Source only QIs for the interface it already holds */
    return D3D_OK;
}
static HRESULT WINAPI o_GetDevice(obj_t *o, IDirect3DDevice9 **out)
{
    InterlockedIncrement(&o->dev->ref);
    *out = (IDirect3DDevice9 *)o->dev;
    return D3D_OK;
}
static D3DRESOURCETYPE WINAPI o_GetType(obj_t *o) { return o->type; }
static DWORD WINAPI o_GetPriority(obj_t *o) { return 0; }
static DWORD WINAPI o_SetPriority(obj_t *o, DWORD p) { return 0; }
static DWORD WINAPI o_GetLevelCount(obj_t *o) { return o->levels; }
static DWORD WINAPI o_GetLOD(obj_t *o) { return 0; }
static DWORD WINAPI o_SetLOD(obj_t *o, DWORD l) { return 0; }
static D3DTEXTUREFILTERTYPE WINAPI o_GetAutoGenFilterType(obj_t *o) { return D3DTEXF_LINEAR; }

static int wants_upload(obj_t *s);
struct device;
static HRESULT gpu_surface(struct device *d, D3DFORMAT fmt, UINT w, UINT h, DWORD usage, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms, struct obj **out);

/* lazily committed backing store */
static BYTE *o_mem(obj_t *o)
{
    if (!o->mem) {
        BYTE *m = VirtualAlloc(NULL, o->memsize ? o->memsize : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (InterlockedCompareExchangePointer((void *volatile *)&o->mem, m, NULL)) VirtualFree(m, 0, MEM_RELEASE);
    }
    return o->mem;
}

/* surfaces + volumes */
static obj_t *new_surface(struct device *dev, obj_t *container, D3DFORMAT fmt, UINT w, UINT h, DWORD usage,
                          D3DPOOL pool, D3DMULTISAMPLE_TYPE ms)
{
    obj_t *s = new_obj(v_surf, dev, D3DRTYPE_SURFACE);
    s->container = container; s->fmt = fmt; s->w = w; s->h = h; s->usage = usage; s->pool = pool; s->ms = ms;
    s->pitch = pitch_of(fmt, w); s->memsize = (SIZE_T)s->pitch * rows_of(fmt, h);
    return s;
}
static HRESULT WINAPI surf_GetDesc(obj_t *s, D3DSURFACE_DESC *d)
{
    d->Format = s->fmt; d->Type = D3DRTYPE_SURFACE; d->Usage = s->usage; d->Pool = s->pool;
    d->MultiSampleType = s->ms; d->MultiSampleQuality = 0; d->Width = s->w; d->Height = s->h;
    return D3D_OK;
}
static HRESULT WINAPI surf_LockRect(obj_t *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD flags)
{
    BYTE *m = o_mem(s);
    if (r) {
        int bc = is_bc8(s->fmt) || is_bc16(s->fmt);
        m += (SIZE_T)(bc ? r->top / 4 : r->top) * s->pitch
           + (SIZE_T)(bc ? (UINT)(r->left / 4) * (is_bc8(s->fmt) ? 8u : 16u) : (UINT)r->left * bpp(s->fmt));
    }
    lr->pBits = m; lr->Pitch = (INT)s->pitch;
    if (!(flags & D3DLOCK_READONLY) && wants_upload(s)) {
        s->upload_on_unlock = 1;
        if (r) s->lrect = *r;
        else { s->lrect.left = 0; s->lrect.top = 0; s->lrect.right = (LONG)s->w; s->lrect.bottom = (LONG)s->h; }
    }
    return D3D_OK;
}
/* ---- M5 GPU uploads */
static uint64_t g_upload_fail;
static void gpu_upload(UINT32 h, UINT face, UINT level, UINT x, UINT y, UINT z, UINT w, UINT hgt, UINT dep,
                       const BYTE *src, UINT pitch, UINT slice)
{
    struct tf2mt_upload_params p = {h, face, level, 0, x, y, z, w, hgt, dep, pitch, slice, (UINT64)(ULONG_PTR)src, 0, 0};
    if (unix_call(TF2MT_UNIX_UPLOAD, &p) || p.status) {
        if (g_upload_fail++ < 20) logmsg("upload failed: handle %x face %u level %u %ux%u status %u\n", h, face, level, w, hgt, p.status);
    }
}
/* address of pixel (x, y) in a CPU subresource */
static BYTE *sub_ptr(obj_t *s, UINT x, UINT y)
{
    int bc = is_bc8(s->fmt) || is_bc16(s->fmt);
    return o_mem(s) + (SIZE_T)(bc ? y / 4 : y) * s->pitch + (SIZE_T)(bc ? (x / 4) * (is_bc8(s->fmt) ? 8u : 16u) : x * bpp(s->fmt));
}
/* the GPU object behind a surface: its own handle or its container's subresource */
static UINT32 surf_gpu(obj_t *s, UINT *face, UINT *level)
{
    *face = s->face; *level = s->level;
    return s->container ? s->container->gpu : s->gpu;
}
/* upload a whole CPU subresource (level surface or volume) into a GPU subresource */
static void upload_sub(obj_t *src, UINT32 dst, UINT face, UINT level)
{
    if (!src->mem) return;   /* never written: nothing to upload */
    gpu_upload(dst, face, level, 0, 0, 0, src->w, src->h, src->d ? src->d : 1, src->mem, src->pitch, src->slice);
}

static HRESULT WINAPI surf_UnlockRect(obj_t *s)
{
    if (s->upload_on_unlock) {
        UINT face, level;
        UINT32 h = surf_gpu(s, &face, &level);
        RECT *r = &s->lrect;
        gpu_upload(h, face, level, (UINT)r->left, (UINT)r->top, 0, (UINT)(r->right - r->left), (UINT)(r->bottom - r->top), 1,
                   sub_ptr(s, (UINT)r->left, (UINT)r->top), s->pitch, 0);
        s->upload_on_unlock = 0;
    }
    return D3D_OK;
}
static HRESULT WINAPI volume_UnlockBox(obj_t *v)
{
    if (v->upload_on_unlock) {
        D3DBOX *b = &v->lbox;
        BYTE *src = o_mem(v) + (SIZE_T)b->Front * v->slice + (SIZE_T)b->Top * v->pitch + (SIZE_T)b->Left * bpp(v->fmt);
        gpu_upload(v->container->gpu, 0, v->level, b->Left, b->Top, b->Front, b->Right - b->Left, b->Bottom - b->Top,
                   b->Back - b->Front, src, v->pitch, v->slice);
        v->upload_on_unlock = 0;
    }
    return D3D_OK;
}
/* locks on GPU-backed subresources whose contents must be uploaded: DYNAMIC and MANAGED textures */
static int wants_upload(obj_t *s)
{
    obj_t *c = s->container;
    return c && c->gpu && !(c->usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
        && ((c->usage & D3DUSAGE_DYNAMIC) || c->pool == D3DPOOL_MANAGED);
}

static HRESULT WINAPI surf_GetContainer(obj_t *s, REFIID riid, void **out)
{
    obj_t *c = s->container;
    if (c) { o_AddRef(c); *out = c; }
    else { InterlockedIncrement(&s->dev->ref); *out = s->dev; }
    return D3D_OK;
}
static HRESULT WINAPI volume_GetDesc(obj_t *v, D3DVOLUME_DESC *d)
{
    d->Format = v->fmt; d->Type = D3DRTYPE_VOLUME; d->Usage = v->usage; d->Pool = v->pool;
    d->Width = v->w; d->Height = v->h; d->Depth = v->d;
    return D3D_OK;
}
static HRESULT WINAPI volume_LockBox(obj_t *v, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD flags)
{
    BYTE *m = o_mem(v);
    if (b) m += (SIZE_T)b->Front * v->slice + (SIZE_T)b->Top * v->pitch + (SIZE_T)b->Left * bpp(v->fmt);
    lb->pBits = m; lb->RowPitch = (INT)v->pitch; lb->SlicePitch = (INT)v->slice;
    if (!(flags & D3DLOCK_READONLY) && wants_upload(v)) {
        v->upload_on_unlock = 1;
        if (b) v->lbox = *b;
        else { D3DBOX all = {0, 0, v->w, v->h, 0, v->d}; v->lbox = all; }
    }
    return D3D_OK;
}

/* textures */
static UINT full_chain(UINT w, UINT h, UINT d)
{
    UINT m = w > h ? w : h, n = 1;
    if (d > m) m = d;
    while (m > 1) { m >>= 1; n++; }
    return n;
}
static obj_t *new_texture(struct device *dev, void **vtbl, D3DRESOURCETYPE type, UINT w, UINT h, UINT d, UINT levels,
                          DWORD usage, D3DFORMAT fmt, D3DPOOL pool)
{
    obj_t *t = new_obj(vtbl, dev, type);
    t->w = w; t->h = h; t->d = d; t->usage = usage; t->fmt = fmt; t->pool = pool;
    if (usage & D3DUSAGE_AUTOGENMIPMAP) levels = 1;
    t->levels = levels ? levels : full_chain(w, h, d);
    UINT faces = type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
    t->nsubs = faces * t->levels;
    t->subs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(obj_t *) * t->nsubs);
    for (UINT f = 0; f < faces; f++)
        for (UINT l = 0; l < t->levels; l++) {
            UINT lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = d >> l ? d >> l : 1;
            obj_t *s;
            if (type == D3DRTYPE_VOLUMETEXTURE) {
                s = new_obj(v_volume, dev, D3DRTYPE_VOLUME);
                s->container = t; s->fmt = fmt; s->w = lw; s->h = lh; s->d = ld; s->usage = usage; s->pool = pool;
                s->pitch = pitch_of(fmt, lw); s->slice = s->pitch * rows_of(fmt, lh); s->memsize = (SIZE_T)s->slice * ld;
            } else {
                s = new_surface(dev, t, fmt, lw, lh, usage, pool, D3DMULTISAMPLE_NONE);
            }
            s->face = f; s->level = l;
            t->subs[f * t->levels + l] = s;
        }
    return t;
}

/* ---- M5: backend textures */
static UINT samples_of(D3DMULTISAMPLE_TYPE ms) { return ms >= D3DMULTISAMPLE_2_SAMPLES ? (UINT)ms : 1; }
static HRESULT gpu_texture(obj_t *o, UINT type, UINT d, UINT samples)
{
    struct tf2mt_texture_params p = {type, (UINT32)o->fmt, o->w, o->h, d, o->levels ? o->levels : 1, o->usage, samples, 0, 0};
    if (unix_call(TF2MT_UNIX_CREATE_TEXTURE, &p) || p.status) {
        logmsg("CreateTexture: backend refused fmt 0x%x %ux%ux%u levels %u usage 0x%lx status %u\n", (UINT)o->fmt, o->w, o->h,
               d, o->levels, o->usage, p.status);
        return p.status == 1 ? D3DERR_NOTAVAILABLE : D3DERR_OUTOFVIDEOMEMORY;
    }
    o->gpu = p.handle;
    return D3D_OK;
}
static int gpu_pool(D3DPOOL p) { return p == D3DPOOL_DEFAULT || p == D3DPOOL_MANAGED; }
static HRESULT finish_texture(obj_t *t, UINT type, obj_t **out)
{
    if (gpu_pool(t->pool)) {
        HRESULT hr = gpu_texture(t, type, t->d, 1);
        if (FAILED(hr)) { free_obj(t); *out = NULL; return hr; }
    }
    *out = t;
    return D3D_OK;
}
static HRESULT WINAPI tex_GetLevelDesc(obj_t *t, UINT l, D3DSURFACE_DESC *d)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return surf_GetDesc(t->subs[l], d);
}
static HRESULT WINAPI tex_GetSurfaceLevel(obj_t *t, UINT l, obj_t **out)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    o_AddRef(t); *out = t->subs[l];
    return D3D_OK;
}
static HRESULT WINAPI tex_LockRect(obj_t *t, UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return surf_LockRect(t->subs[l], lr, r, f);
}
static HRESULT WINAPI tex_UnlockRect(obj_t *t, UINT l)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return surf_UnlockRect(t->subs[l]);
}
static HRESULT WINAPI cube_UnlockRect(obj_t *t, D3DCUBEMAP_FACES face, UINT l)
{
    if (l >= t->levels || (UINT)face > 5) return D3DERR_INVALIDCALL;
    return surf_UnlockRect(t->subs[face * t->levels + l]);
}
static HRESULT WINAPI vol_UnlockBox(obj_t *t, UINT l)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return volume_UnlockBox(t->subs[l]);
}
static HRESULT WINAPI cube_GetCubeMapSurface(obj_t *t, D3DCUBEMAP_FACES face, UINT l, obj_t **out)
{
    if (l >= t->levels || (UINT)face > 5) return D3DERR_INVALIDCALL;
    o_AddRef(t); *out = t->subs[face * t->levels + l];
    return D3D_OK;
}
static HRESULT WINAPI cube_LockRect(obj_t *t, D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    if (l >= t->levels || (UINT)face > 5) return D3DERR_INVALIDCALL;
    return surf_LockRect(t->subs[face * t->levels + l], lr, r, f);
}
static HRESULT WINAPI vol_GetLevelDesc(obj_t *t, UINT l, D3DVOLUME_DESC *d)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return volume_GetDesc(t->subs[l], d);
}
static HRESULT WINAPI vol_GetVolumeLevel(obj_t *t, UINT l, obj_t **out)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    o_AddRef(t); *out = t->subs[l];
    return D3D_OK;
}
static HRESULT WINAPI vol_LockBox(obj_t *t, UINT l, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD f)
{
    if (l >= t->levels) return D3DERR_INVALIDCALL;
    return volume_LockBox(t->subs[l], lb, b, f);
}

/* buffers */
/* Buffers live in shared Metal memory: Lock is pointer arithmetic. DISCARD on a dynamic buffer renames it to a fresh
 * backing so in-flight GPU reads of the old contents stay valid (PLAN §8.1 locking model). */
static uint64_t g_discards;
static HRESULT WINAPI buf_Lock(obj_t *b, UINT off, UINT size, void **p, DWORD flags)
{
    if (b->gpu && (flags & D3DLOCK_DISCARD) && (b->usage & D3DUSAGE_DYNAMIC)) {
        struct tf2mt_buffer_params r = {b->w, 1, b->gpu, 0, 0, 0, 0};
        /* the new backing is ours to write at once; queued draws keep the old one until the switch in the stream */
        if (!unix_call(TF2MT_UNIX_RENAME_BUFFER, &r) && !r.status) {
            b->mem = (BYTE *)(ULONG_PTR)r.cpu;
            CMD(TF2MT_CMD_BUFFER_SWITCH, b->gpu, r.token);
        }
        g_discards++;
    }
    *p = o_mem(b) + off;
    return D3D_OK;
}
static HRESULT gpu_buffer(obj_t *b)
{
    struct tf2mt_buffer_params p = {b->w, (b->usage & D3DUSAGE_DYNAMIC) != 0, 0, 0, 0, 0, 0};
    if (unix_call(TF2MT_UNIX_CREATE_BUFFER, &p) || p.status) {
        logmsg("CreateBuffer: backend refused %u bytes (status %u)\n", b->w, p.status);
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    b->gpu = p.handle; b->gpu_mem = 1; b->mem = (BYTE *)(ULONG_PTR)p.cpu;
    return D3D_OK;
}
static HRESULT WINAPI vb_GetDesc(obj_t *b, D3DVERTEXBUFFER_DESC *d)
{
    d->Format = D3DFMT_VERTEXDATA; d->Type = D3DRTYPE_VERTEXBUFFER; d->Usage = b->usage; d->Pool = b->pool;
    d->Size = b->w; d->FVF = b->fvf;
    return D3D_OK;
}
static HRESULT WINAPI ib_GetDesc(obj_t *b, D3DINDEXBUFFER_DESC *d)
{
    d->Format = b->fmt; d->Type = D3DRTYPE_INDEXBUFFER; d->Usage = b->usage; d->Pool = b->pool; d->Size = b->w;
    return D3D_OK;
}

/* declarations, shaders, queries, state blocks */
static HRESULT WINAPI decl_GetDeclaration(obj_t *o, D3DVERTEXELEMENT9 *e, UINT *n)
{
    UINT cnt = o->blobsize / sizeof(D3DVERTEXELEMENT9);
    if (e) memcpy(e, o->blob, o->blobsize);
    *n = cnt;
    return D3D_OK;
}
static HRESULT WINAPI shader_GetFunction(obj_t *o, void *data, UINT *size)
{
    if (data) memcpy(data, o->blob, o->blobsize);
    *size = o->blobsize;
    return D3D_OK;
}
static D3DQUERYTYPE WINAPI q_GetType(obj_t *q) { return q->qtype; }
static DWORD WINAPI q_GetDataSize(obj_t *q)
{
    switch (q->qtype) {
    case D3DQUERYTYPE_EVENT: return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION: return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP: return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ: return sizeof(UINT64);
    default: return 0;
    }
}
/* M7 queries: OCCLUSION counts samples on the GPU (visibility results), EVENT completes with the command buffer that
 * carries it. Each Issue takes a fresh result slot, so results of earlier issues stay readable. `level` = slot + 1
 * (0 = never issued), `face` = an open BEGIN. */
static volatile LONG g_next_slot;
/* q->d holds the issue generation (the full counter): a reused slot is only "done" for the matching generation */
static UINT32 new_slot(obj_t *q) { LONG g = InterlockedIncrement(&g_next_slot); q->d = (UINT)g; return (UINT32)g % TF2MT_QUERY_SLOTS; }
static void cmd_flush(void);
static HRESULT WINAPI q_Issue(obj_t *q, DWORD flags)
{
    if (q->qtype == D3DQUERYTYPE_OCCLUSION) {
        if (flags & D3DISSUE_BEGIN) { q->level = new_slot(q) + 1; q->face = 1; CMD(TF2MT_CMD_QUERY_BEGIN, q->level - 1, q->d); }
        if ((flags & D3DISSUE_END) && q->level) { q->face = 0; CMD(TF2MT_CMD_QUERY_END, q->level - 1, q->d); }
    } else if (q->qtype == D3DQUERYTYPE_EVENT && (flags & D3DISSUE_END)) {
        q->level = new_slot(q) + 1;
        CMD(TF2MT_CMD_EVENT, q->level - 1, q->d);
    }
    return D3D_OK;
}
static HRESULT WINAPI q_GetData(obj_t *q, void *data, DWORD size, DWORD flags)
{
    if (data && size) memset(data, 0, size);
    if (q->qtype == D3DQUERYTYPE_EVENT || q->qtype == D3DQUERYTYPE_OCCLUSION) {
        if (!q->level) { if (data && size) *(DWORD *)data = q->qtype == D3DQUERYTYPE_EVENT ? TRUE : 0; return D3D_OK; }
        if (q->face) return S_FALSE;                     /* BEGIN without END */
        if (flags & D3DGETDATA_FLUSH) cmd_flush();       /* FLUSH: hand the END to the encoder (it drains) */
        struct tf2mt_query_params p = {q->level - 1, (flags & D3DGETDATA_FLUSH) != 0, 0, q->d, 0};
        if (unix_call(TF2MT_UNIX_QUERY_DATA, &p) || !p.done) return S_FALSE;
        if (data && size >= sizeof(DWORD))
            *(DWORD *)data = q->qtype == D3DQUERYTYPE_EVENT ? TRUE : (DWORD)(p.value > 0xffffffffull ? 0xffffffffu : p.value);
        return D3D_OK;
    }
    if (!data || !size) return D3D_OK;
    if (q->qtype == D3DQUERYTYPE_TIMESTAMPFREQ && size >= 8) *(UINT64 *)data = 1000000000ull;
    return D3D_OK;
}

/* swap chain (implicit only) */
static HRESULT WINAPI sc_GetBackBuffer(obj_t *sc, UINT i, D3DBACKBUFFER_TYPE t, obj_t **out)
{
    obj_t *bb = sc->dev->backbuffer;
    o_AddRef(bb); *out = bb;
    return D3D_OK;
}
static HRESULT WINAPI sc_GetPresentParameters(obj_t *sc, D3DPRESENT_PARAMETERS *pp)
{
    *pp = sc->dev->pp;
    return D3D_OK;
}

/* ---------------------------------------------------------------- device */
typedef struct device dev_t;
static HRESULT WINAPI dev_QueryInterface(dev_t *d, REFIID riid, void **out)
{
    InterlockedIncrement(&d->ref); *out = d;
    return D3D_OK;
}
static ULONG WINAPI dev_AddRef(dev_t *d) { return (ULONG)InterlockedIncrement(&d->ref); }
static ULONG WINAPI dev_Release(dev_t *d)
{
    LONG r = InterlockedDecrement(&d->ref);
    if (!r) logmsg("device released\n"); /* leaked on purpose: throwaway, process is exiting */
    return (ULONG)r;
}
static UINT WINAPI dev_GetAvailableTextureMem(dev_t *d) { return 0xC0000000u; }
static HRESULT WINAPI dev_GetDirect3D(dev_t *d, IDirect3D9 **out)
{
    IDirect3D9Ex_AddRef(d->d3d); *out = (IDirect3D9 *)d->d3d;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetDeviceCaps(dev_t *d, D3DCAPS9 *caps)
{
    return tf2mt_adapter_caps(caps);
}
static HRESULT WINAPI dev_GetDisplayMode(dev_t *d, UINT sc, D3DDISPLAYMODE *m)
{
    return tf2mt_adapter_display_mode(m);
}
static HRESULT WINAPI dev_GetCreationParameters(dev_t *d, D3DDEVICE_CREATION_PARAMETERS *cp)
{
    *cp = d->cp;
    return D3D_OK;
}
static BOOL WINAPI dev_ShowCursor(dev_t *d, BOOL show) { return TRUE; }
static HRESULT WINAPI dev_GetSwapChain(dev_t *d, UINT i, obj_t **out)
{
    o_AddRef(d->swapchain); *out = d->swapchain;
    return D3D_OK;
}
static UINT WINAPI dev_GetNumberOfSwapChains(dev_t *d) { return 1; }

static void set_ref(obj_t **slot, obj_t *o)
{
    if (o) o_AddRef(o);
    obj_t *old = *slot;
    *slot = o;
    if (old) o_Release(old);
}

static void make_backbuffers(dev_t *d)
{
    D3DPRESENT_PARAMETERS *pp = &d->pp;
    if (!pp->BackBufferWidth || !pp->BackBufferHeight) {
        RECT r = {0, 0, 1920, 1080};
        if (pp->hDeviceWindow) GetClientRect(pp->hDeviceWindow, &r);
        pp->BackBufferWidth = r.right - r.left; pp->BackBufferHeight = r.bottom - r.top;
    }
    if (pp->BackBufferFormat == D3DFMT_UNKNOWN) pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    if (!pp->BackBufferCount) pp->BackBufferCount = 1;
    set_ref(&d->rt[0], NULL); set_ref(&d->ds, NULL);
    if (d->backbuffer) o_Release(d->backbuffer);
    if (d->autods) o_Release(d->autods);
    d->backbuffer = d->autods = NULL;
    gpu_surface(d, pp->BackBufferFormat, pp->BackBufferWidth, pp->BackBufferHeight, D3DUSAGE_RENDERTARGET,
                D3DPOOL_DEFAULT, pp->MultiSampleType, &d->backbuffer);
    if (pp->EnableAutoDepthStencil)
        gpu_surface(d, pp->AutoDepthStencilFormat, pp->BackBufferWidth, pp->BackBufferHeight, D3DUSAGE_DEPTHSTENCIL,
                    D3DPOOL_DEFAULT, pp->MultiSampleType, &d->autods);
    if (!d->backbuffer)   /* backend unavailable: keep a CPU placeholder so the game still runs */
        d->backbuffer = new_surface(d, NULL, pp->BackBufferFormat, pp->BackBufferWidth, pp->BackBufferHeight,
                                    D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, pp->MultiSampleType);
    set_ref(&d->rt[0], d->backbuffer);
    if (d->autods) set_ref(&d->ds, d->autods);
}

static void attach_window(dev_t *d)
{
    struct tf2mt_attach_params a = { (UINT64)(ULONG_PTR)(d->pp.hDeviceWindow ? d->pp.hDeviceWindow : d->cp.hFocusWindow),
                                     d->pp.BackBufferWidth, d->pp.BackBufferHeight, (UINT32)g_vsync };
    if (g_attached) unix_call(TF2MT_UNIX_DETACH, NULL);
    g_attached = unix_call(TF2MT_UNIX_ATTACH, &a) == 0;
    logmsg("attach hwnd %p %ux%u -> %s\n", (void *)(ULONG_PTR)a.hwnd, a.width, a.height, g_attached ? "ok" : "FAILED");
}

static void emit_device_defaults(dev_t *d);
static HRESULT WINAPI dev_Reset(dev_t *d, D3DPRESENT_PARAMETERS *pp)
{
    cmd_flush();
    d->pp = *pp;
    make_backbuffers(d);
    *pp = d->pp;
    attach_window(d);
    emit_device_defaults(d);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetBackBuffer(dev_t *d, UINT sc, UINT i, D3DBACKBUFFER_TYPE t, obj_t **out)
{
    o_AddRef(d->backbuffer); *out = d->backbuffer;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRasterStatus(dev_t *d, UINT sc, D3DRASTER_STATUS *rs)
{
    rs->InVBlank = FALSE; rs->ScanLine = 0;
    return D3D_OK;
}
static void WINAPI dev_GetGammaRamp(dev_t *d, UINT sc, D3DGAMMARAMP *r)
{
    for (int i = 0; i < 256; i++) r->red[i] = r->green[i] = r->blue[i] = (WORD)(i * 257);
}

/* D3D9Ex user-memory textures: for D3DPOOL_SYSTEMMEM, a non-NULL *pSharedHandle is a pointer to memory the
 * application owns and writes directly (no LockRect); the texture must have one level. TF2 uploads every no-mip
 * texture this way (sky, HUD, crosshair, UI images, item icons) and then calls UpdateSurface. Rows use the
 * runtime's pitch for that width, 4-byte aligned for uncompressed formats. */
static void use_user_memory(obj_t *s, void *mem)
{
    if (!is_bc8(s->fmt) && !is_bc16(s->fmt)) s->pitch = (s->w * bpp(s->fmt) + 3) & ~3u;
    s->memsize = (SIZE_T)s->pitch * rows_of(s->fmt, s->h);
    s->mem = mem;
    s->gpu_mem = 1;   /* not ours: never freed here */
}
static HRESULT WINAPI dev_CreateTexture(dev_t *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                        D3DPOOL pool, obj_t **out, HANDLE *shared)
{
    obj_t *t = new_texture(d, v_tex, D3DRTYPE_TEXTURE, w, h, 1, levels, usage, fmt, pool);
    if (pool == D3DPOOL_SYSTEMMEM && shared && *shared) {
        if (t->levels != 1) { free_obj(t); *out = NULL; return D3DERR_INVALIDCALL; }
        use_user_memory(t->subs[0], *shared);
    }
    return finish_texture(t, TF2MT_TEX_2D, out);
}
static HRESULT WINAPI dev_CreateVolumeTexture(dev_t *d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                              D3DFORMAT fmt, D3DPOOL pool, obj_t **out, HANDLE *shared)
{
    return finish_texture(new_texture(d, v_vol, D3DRTYPE_VOLUMETEXTURE, w, h, depth, levels, usage, fmt, pool), TF2MT_TEX_3D, out);
}
static HRESULT WINAPI dev_CreateCubeTexture(dev_t *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                            D3DPOOL pool, obj_t **out, HANDLE *shared)
{
    return finish_texture(new_texture(d, v_cube, D3DRTYPE_CUBETEXTURE, edge, edge, 1, levels, usage, fmt, pool), TF2MT_TEX_CUBE, out);
}
static HRESULT new_buffer(dev_t *d, void **vtbl, D3DRESOURCETYPE type, UINT len, DWORD usage, D3DFORMAT fmt, DWORD fvf,
                          D3DPOOL pool, obj_t **out)
{
    obj_t *b = new_obj(vtbl, d, type);
    b->w = len; b->memsize = len; b->usage = usage; b->fmt = fmt; b->fvf = fvf; b->pool = pool;
    if (pool != D3DPOOL_SCRATCH) {
        HRESULT hr = gpu_buffer(b);
        if (FAILED(hr)) { free_obj(b); *out = NULL; return hr; }
    }
    *out = b;
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateVertexBuffer(dev_t *d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                             obj_t **out, HANDLE *shared)
{
    return new_buffer(d, v_vb, D3DRTYPE_VERTEXBUFFER, len, usage, D3DFMT_VERTEXDATA, fvf, pool, out);
}
static HRESULT WINAPI dev_CreateIndexBuffer(dev_t *d, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                            obj_t **out, HANDLE *shared)
{
    return new_buffer(d, v_ib, D3DRTYPE_INDEXBUFFER, len, usage, fmt, 0, pool, out);
}
/* standalone GPU surface (render target, depth-stencil, DEFAULT-pool offscreen, back buffer) */
static HRESULT gpu_surface(dev_t *d, D3DFORMAT fmt, UINT w, UINT h, DWORD usage, D3DPOOL pool, D3DMULTISAMPLE_TYPE ms, obj_t **out)
{
    obj_t *s = new_surface(d, NULL, fmt, w, h, usage, pool, ms);
    s->levels = 1;
    if (gpu_pool(pool)) {
        HRESULT hr = gpu_texture(s, TF2MT_TEX_2D, 1, samples_of(ms));
        if (FAILED(hr)) { free_obj(s); *out = NULL; return hr; }
    }
    *out = s;
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateRenderTarget(dev_t *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                             DWORD q, BOOL lockable, obj_t **out, HANDLE *shared)
{
    return gpu_surface(d, fmt, w, h, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ms, out);
}
static HRESULT WINAPI dev_CreateDepthStencilSurface(dev_t *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                    DWORD q, BOOL discard, obj_t **out, HANDLE *shared)
{
    return gpu_surface(d, fmt, w, h, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ms, out);
}
static HRESULT WINAPI dev_CreateOffscreenPlainSurface(dev_t *d, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                      obj_t **out, HANDLE *shared)
{
    /* DEFAULT-pool offscreen surfaces are StretchRect / ColorFill targets: give them render-target usage */
    HRESULT hr = gpu_surface(d, fmt, w, h, pool == D3DPOOL_DEFAULT ? D3DUSAGE_RENDERTARGET : 0, pool, D3DMULTISAMPLE_NONE, out);
    if (SUCCEEDED(hr) && pool == D3DPOOL_SYSTEMMEM && shared && *shared) use_user_memory(*out, *shared);
    return hr;
}

/* ---- uploads: UpdateTexture / UpdateSurface (census "Uploads": the only texture upload paths TF2 uses) */
static HRESULT WINAPI dev_UpdateTexture(dev_t *d, obj_t *src, obj_t *dst)
{
    if (!src || !dst || !dst->gpu || src->type != dst->type) return D3DERR_INVALIDCALL;
    UINT faces = dst->type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
    if (src->levels < dst->levels) return D3DERR_INVALIDCALL;
    UINT skip = src->levels - dst->levels;   /* D3D9: the destination's top level matches the source level of equal size */
    for (UINT f = 0; f < faces; f++)
        for (UINT l = 0; l < dst->levels; l++)
            upload_sub(src->subs[f * src->levels + l + skip], dst->gpu, f, l);
    return D3D_OK;
}
static HRESULT WINAPI dev_UpdateSurface(dev_t *d, obj_t *src, const RECT *sr, obj_t *dst, const POINT *pt)
{
    if (!src || !dst) return D3DERR_INVALIDCALL;
    UINT face, level;
    UINT32 h = surf_gpu(dst, &face, &level);
    if (!h || !src->mem) return h ? D3D_OK : D3DERR_INVALIDCALL;
    RECT r = {0, 0, (LONG)src->w, (LONG)src->h};
    if (sr) r = *sr;
    UINT x = pt ? (UINT)pt->x : (UINT)r.left, y = pt ? (UINT)pt->y : (UINT)r.top;
    gpu_upload(h, face, level, x, y, 0, (UINT)(r.right - r.left), (UINT)(r.bottom - r.top), 1,
               sub_ptr(src, (UINT)r.left, (UINT)r.top), src->pitch, 0);
    return D3D_OK;
}
/* GetRenderTargetData: synchronous GPU -> system-memory copy (screenshots) */
static HRESULT WINAPI dev_GetRenderTargetData(dev_t *d, obj_t *rt, obj_t *dst)
{
    if (!rt || !dst) return D3DERR_INVALIDCALL;
    UINT face, level;
    UINT32 h = surf_gpu(rt, &face, &level);
    if (!h) return D3DERR_INVALIDCALL;
    struct tf2mt_upload_params p = {h, face, level, 0, 0, 0, 0, dst->w, dst->h, 1, dst->pitch, 0, (UINT64)(ULONG_PTR)o_mem(dst), 0, 0};
    cmd_flush();   /* render everything queued before reading back */
    if (unix_call(TF2MT_UNIX_READBACK, &p) || p.status) {
        static int once;
        if (!once++) logmsg("GetRenderTargetData: readback unsupported for this surface (status %u)\n", p.status);
    }
    return D3D_OK;   /* contents may be stale until M6 renders; the call itself never fails the game */
}
static UINT32 surf_handle(obj_t *s, UINT *face, UINT *level)
{
    *face = *level = 0;
    return s ? surf_gpu(s, face, level) : 0;
}
static void emit_viewport(dev_t *d)
{
    D3DVIEWPORT9 *v = &d->vp;
    UINT32 a[6] = {v->X, v->Y, v->Width, v->Height, 0, 0};
    memcpy(&a[4], &v->MinZ, 4); memcpy(&a[5], &v->MaxZ, 4);
    cmd(TF2MT_CMD_VIEWPORT, a, 6, NULL, 0);
}
static HRESULT WINAPI dev_SetRenderTarget(dev_t *d, DWORD i, obj_t *s)
{
    if (i >= 4 || (i == 0 && !s)) return D3DERR_INVALIDCALL;
    set_ref(&d->rt[i], s);
    UINT face, level;
    UINT32 h = surf_handle(s, &face, &level);
    CMD(TF2MT_CMD_RT, i, h, face, level);
    if (i == 0) {   /* D3D9: setting RT0 resets the viewport (and scissor rect) to the whole target */
        D3DVIEWPORT9 v = {0, 0, s->w, s->h, 0.0f, 1.0f};
        d->vp = v;
        emit_viewport(d);
        RECT r = {0, 0, (LONG)s->w, (LONG)s->h};
        d->scissor = r;
        CMD(TF2MT_CMD_SCISSOR, 0, 0, s->w, s->h);
    }
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderTarget(dev_t *d, DWORD i, obj_t **out)
{
    if (i >= 4) return D3DERR_INVALIDCALL;
    if (!d->rt[i]) { *out = NULL; return D3DERR_NOTFOUND; }
    o_AddRef(d->rt[i]); *out = d->rt[i];
    return D3D_OK;
}
static HRESULT WINAPI dev_SetDepthStencilSurface(dev_t *d, obj_t *s)
{
    set_ref(&d->ds, s);
    UINT face, level;
    CMD(TF2MT_CMD_DS, surf_handle(s, &face, &level));
    return D3D_OK;
}

/* ---- state and draws (M6) */
static DWORD stage_index(DWORD st) { return st >= D3DVERTEXTEXTURESAMPLER0 ? 16 + (st - D3DVERTEXTEXTURESAMPLER0) : st; }
static HRESULT WINAPI dev_SetRenderState(dev_t *d, D3DRENDERSTATETYPE st, DWORD v)
{
    if ((UINT)st < 256) d->rs[st] = v;
    CMD(TF2MT_CMD_RS, (UINT32)st, v);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetRenderState(dev_t *d, D3DRENDERSTATETYPE st, DWORD *v)
{
    *v = (UINT)st < 256 ? d->rs[st] : 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_SetSamplerState(dev_t *d, DWORD smp, D3DSAMPLERSTATETYPE t, DWORD v)
{
    DWORD s = stage_index(smp);
    if (s >= 20) return D3DERR_INVALIDCALL;
    CMD(TF2MT_CMD_SS, s, (UINT32)t, v);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetTexture(dev_t *d, DWORD stage, obj_t *t)
{
    DWORD s = stage_index(stage);
    if (s >= 20) return D3DERR_INVALIDCALL;
    set_ref(&d->tex[s], t);
    CMD(TF2MT_CMD_TEXTURE, s, t ? t->gpu : 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTexture2(dev_t *d, DWORD stage, obj_t **out)
{
    DWORD s = stage_index(stage);
    if (s >= 20) return D3DERR_INVALIDCALL;
    *out = d->tex[s];
    if (*out) o_AddRef(*out);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetStreamSource(dev_t *d, UINT s, obj_t *vb, UINT off, UINT stride)
{
    if (s >= 16) return D3DERR_INVALIDCALL;
    CMD(TF2MT_CMD_STREAM, s, vb ? vb->gpu : 0, off, stride);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetIndices(dev_t *d, obj_t *ib)
{
    CMD(TF2MT_CMD_INDICES, ib ? ib->gpu : 0, ib ? (UINT32)ib->fmt : 101);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetVertexDeclaration(dev_t *d, obj_t *decl) { CMD(TF2MT_CMD_VDECL, decl ? decl->gpu : 0); return D3D_OK; }
static HRESULT WINAPI dev_SetVertexShader(dev_t *d, obj_t *sh) { CMD(TF2MT_CMD_VS, sh ? sh->gpu : 0); return D3D_OK; }
static HRESULT WINAPI dev_SetPixelShader(dev_t *d, obj_t *sh) { CMD(TF2MT_CMD_PS, sh ? sh->gpu : 0); return D3D_OK; }
#define CONSTS(name, op, T, per) \
    static HRESULT WINAPI dev_##name(dev_t *d, UINT start, const T *data, UINT n) \
    { const UINT32 a[2] = {start, n}; cmd(op, a, 2, data, n * (per)); return D3D_OK; }
CONSTS(SetVertexShaderConstantF, TF2MT_CMD_VS_F, float, 4)
CONSTS(SetPixelShaderConstantF, TF2MT_CMD_PS_F, float, 4)
CONSTS(SetVertexShaderConstantI, TF2MT_CMD_VS_I, int, 4)
CONSTS(SetPixelShaderConstantI, TF2MT_CMD_PS_I, int, 4)
CONSTS(SetVertexShaderConstantB, TF2MT_CMD_VS_B, BOOL, 1)
CONSTS(SetPixelShaderConstantB, TF2MT_CMD_PS_B, BOOL, 1)
static HRESULT WINAPI dev_SetViewport(dev_t *d, const D3DVIEWPORT9 *v) { d->vp = *v; emit_viewport(d); return D3D_OK; }
static HRESULT WINAPI dev_GetViewport(dev_t *d, D3DVIEWPORT9 *v) { *v = d->vp; return D3D_OK; }
static HRESULT WINAPI dev_SetScissorRect(dev_t *d, const RECT *r)
{
    d->scissor = *r;
    CMD(TF2MT_CMD_SCISSOR, (UINT32)r->left, (UINT32)r->top, (UINT32)r->right, (UINT32)r->bottom);
    return D3D_OK;
}
static HRESULT WINAPI dev_GetScissorRect(dev_t *d, RECT *r) { *r = d->scissor; return D3D_OK; }
static HRESULT WINAPI dev_SetClipPlane(dev_t *d, DWORD i, const float *p)
{
    if (i >= 6) return D3DERR_INVALIDCALL;
    const UINT32 a[1] = {i};
    cmd(TF2MT_CMD_CLIP_PLANE, a, 1, p, 4);
    return D3D_OK;
}
static HRESULT WINAPI dev_Clear(dev_t *d, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z, DWORD st)
{
    UINT32 a[5] = {flags, c, 0, st, r ? n : 0};
    memcpy(&a[2], &z, 4);
    cmd(TF2MT_CMD_CLEAR, a, 5, r, r ? n * 4 : 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawIndexedPrimitive(dev_t *d, D3DPRIMITIVETYPE t, INT base, UINT mi, UINT nv, UINT start, UINT pc)
{
    CMD(TF2MT_CMD_DRAW_INDEXED, (UINT32)t, (UINT32)base, mi, nv, start, pc);
    return D3D_OK;
}
static HRESULT WINAPI dev_DrawPrimitive(dev_t *d, D3DPRIMITIVETYPE t, UINT start, UINT pc)
{
    CMD(TF2MT_CMD_DRAW, (UINT32)t, start, pc);
    return D3D_OK;
}
static void rect_words(UINT32 *w, const RECT *r)
{
    w[0] = r != NULL;
    if (r) { w[1] = (UINT32)r->left; w[2] = (UINT32)r->top; w[3] = (UINT32)r->right; w[4] = (UINT32)r->bottom; }
    else w[1] = w[2] = w[3] = w[4] = 0;
}
static HRESULT WINAPI dev_StretchRect(dev_t *d, obj_t *src, const RECT *sr, obj_t *dst, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
    if (!src || !dst) return D3DERR_INVALIDCALL;
    UINT32 a[17];
    UINT face, level;
    a[0] = surf_handle(src, &face, &level); a[1] = face; a[2] = level; rect_words(a + 3, sr);
    a[8] = surf_handle(dst, &face, &level); a[9] = face; a[10] = level; rect_words(a + 11, dr);
    a[16] = (UINT32)f;
    cmd(TF2MT_CMD_STRETCH, a, 17, NULL, 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_ColorFill(dev_t *d, obj_t *s, const RECT *r, D3DCOLOR c)
{
    if (!s) return D3DERR_INVALIDCALL;
    UINT32 a[9];
    UINT face, level;
    a[0] = surf_handle(s, &face, &level); a[1] = face; a[2] = level; rect_words(a + 3, r);
    a[8] = c;
    cmd(TF2MT_CMD_COLOR_FILL, a, 9, NULL, 0);
    return D3D_OK;
}
/* D3D9 defaults that depend on the device (render targets, viewport, ZENABLE) after RESET_STATE */
static void emit_device_defaults(dev_t *d)
{
    CMD(TF2MT_CMD_RESET_STATE);
    memset(d->rs, 0, sizeof d->rs);
    d->rs[D3DRS_ZENABLE] = d->pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    d->rs[D3DRS_COLORWRITEENABLE] = 0xf;
    CMD(TF2MT_CMD_RS, D3DRS_ZENABLE, d->rs[D3DRS_ZENABLE]);
    for (int i = 0; i < 20; i++) set_ref(&d->tex[i], NULL);
    dev_SetRenderTarget(d, 0, d->backbuffer);
    dev_SetDepthStencilSurface(d, d->autods);
}
static HRESULT WINAPI dev_GetDepthStencilSurface(dev_t *d, obj_t **out)
{
    if (!d->ds) { *out = NULL; return D3DERR_NOTFOUND; }
    o_AddRef(d->ds); *out = d->ds;
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateStateBlock(dev_t *d, D3DSTATEBLOCKTYPE t, obj_t **out)
{
    *out = new_obj(v_sb, d, 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_EndStateBlock(dev_t *d, obj_t **out)
{
    *out = new_obj(v_sb, d, 0);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateVertexDeclaration(dev_t *d, const D3DVERTEXELEMENT9 *e, obj_t **out)
{
    UINT n = 0;
    while (e[n].Stream != 0xff) n++;
    n++; /* include D3DDECL_END */
    obj_t *o = new_obj(v_decl, d, 0);
    o->blobsize = n * sizeof *e;
    o->blob = HeapAlloc(GetProcessHeap(), 0, o->blobsize);
    memcpy(o->blob, e, o->blobsize);
    struct tf2mt_decl_params dp = {(UINT64)(ULONG_PTR)o->blob, n, 0};
    if (!unix_call(TF2MT_UNIX_CREATE_DECL, &dp)) o->gpu = dp.handle;
    *out = o;
    return D3D_OK;
}
static obj_t *new_shader(dev_t *d, void **vtbl, const DWORD *code)
{
    UINT n = 1;
    while (code[n] != 0x0000ffff) {
        DWORD tok = code[n];
        if ((tok & 0xffff) == 0xfffe) n += 1 + (tok >> 16);  /* comment block */
        else n++;
    }
    n++;
    obj_t *o = new_obj(vtbl, d, 0);
    o->blobsize = n * 4;
    o->blob = HeapAlloc(GetProcessHeap(), 0, o->blobsize);
    memcpy(o->blob, code, o->blobsize);
    struct tf2mt_shader_params sp = {(UINT64)(ULONG_PTR)o->blob, n, 0, 0, 0};
    if (!unix_call(TF2MT_UNIX_CREATE_SHADER, &sp)) o->gpu = sp.handle;   /* status != 0: logged by the backend */
    return o;
}
static HRESULT WINAPI dev_CreateVertexShader(dev_t *d, const DWORD *code, obj_t **out)
{
    *out = new_shader(d, v_vs, code);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreatePixelShader(dev_t *d, const DWORD *code, obj_t **out)
{
    *out = new_shader(d, v_ps, code);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateQuery(dev_t *d, D3DQUERYTYPE t, obj_t **out)
{
    switch (t) {
    case D3DQUERYTYPE_EVENT: case D3DQUERYTYPE_OCCLUSION: case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT: case D3DQUERYTYPE_TIMESTAMPFREQ: break;
    default: return D3DERR_NOTAVAILABLE;
    }
    if (!out) return D3D_OK; /* support check */
    obj_t *q = new_obj(v_query, d, 0);
    q->qtype = t;
    *out = q;
    return D3D_OK;
}
static int g_nopresent;   /* TF2MT_NOPRESENT=1: accept Present but never touch Metal (measures the null-device cost alone) */
/* per-frame log for owner-run benchmarks (online matches): TF2MT_FRAME_LOG=<windows path> -> CSV "t_s,frame_ms",
 * written in 600-frame batches (one WriteFile each) */
static HANDLE g_framelog = INVALID_HANDLE_VALUE;
static char g_framebuf[600 * 48];
static size_t g_framebuf_len;
static void framelog(double t_s, double ms)
{
    static int init;
    if (!init) {
        char path[MAX_PATH];
        init = 1;
        if (GetEnvironmentVariableA("TF2MT_FRAME_LOG", path, sizeof path)) {
            g_framelog = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            DWORD w;
            if (g_framelog != INVALID_HANDLE_VALUE) WriteFile(g_framelog, "t_s,frame_ms,layer_ms,present_ms\n", 33, &w, NULL);
        }
    }
    if (g_framelog == INVALID_HANDLE_VALUE) return;
    double f = 1000.0 / (double)g_freq.QuadPart;
    LONG64 lt = InterlockedExchange64(&g_layer_ticks, 0), pt = InterlockedExchange64(&g_present_ticks, 0);
    g_framebuf_len += (size_t)snprintf(g_framebuf + g_framebuf_len, sizeof g_framebuf - g_framebuf_len, "%.3f,%.3f,%.3f,%.3f\n",
                                       t_s, ms, lt * f, pt * f);
    if (g_framebuf_len > sizeof g_framebuf - 32) {
        DWORD w;
        WriteFile(g_framelog, g_framebuf, (DWORD)g_framebuf_len, &w, NULL);
        g_framebuf_len = 0;
    }
}

static void present_stats(void)
{
    static LARGE_INTEGER last; static unsigned n; static double sum, mx;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        double ms = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
        framelog((double)(now.QuadPart - g_t0.QuadPart) / (double)g_freq.QuadPart, ms);
        sum += ms; if (ms > mx) mx = ms;
        if (++n == 600) { logmsg("present(pe): 600 frames mean %.3f ms (%.1f fps) max %.2f\n", sum / 600, 600000.0 / sum, mx); n = 0; sum = mx = 0; }
    }
    last = now;
}

/* upload ledger (PLAN §11): one line every 600 frames */
static void ledger(void)
{
    static unsigned n;
    static struct tf2mt_stats prev;
    if (++n % 600) return;
    struct tf2mt_stats st = {0};
    if (unix_call(TF2MT_UNIX_STATS, &st)) return;
    logmsg("ledger: uploads %.1f MB in %llu calls (%.1f ms in upload calls), discards %llu (renames %llu, new backings %llu); "
           "live: %llu buffers %.0f MB, %llu textures %.0f MB; verify ok %llu fail %llu\n",
           (st.upload_bytes - prev.upload_bytes) / 1e6, (unsigned long long)(st.upload_calls - prev.upload_calls),
           (st.upload_ns - prev.upload_ns) / 1e6, (unsigned long long)g_discards,
           (unsigned long long)st.renames, (unsigned long long)st.rename_allocs,
           (unsigned long long)st.buffers_live, st.buffer_bytes_live / 1e6, (unsigned long long)st.textures_live,
           st.texture_bytes_live / 1e6, (unsigned long long)st.verify_ok, (unsigned long long)st.verify_fail);
    prev = st;
}

static HRESULT WINAPI dev_Present(dev_t *d, const RECT *s, const RECT *t, HWND w, const RGNDATA *r)
{
    present_stats();
    ledger();
    UINT face, level;
    if (!g_nopresent) CMD(TF2MT_CMD_PRESENT, surf_handle(d->backbuffer, &face, &level));
    g_in_present = 1;
    cmd_flush();
    g_in_present = 0;
    return D3D_OK;
}
static HRESULT WINAPI dev_GetTexture(dev_t *d, DWORD stage, obj_t **out) { *out = NULL; return D3D_OK; }
static HRESULT WINAPI dev_GetFVF(dev_t *d, DWORD *fvf) { *fvf = 0; return D3D_OK; }

/* ---------------------------------------------------------------- device creation (called by adapter.c) */
HRESULT tf2mt_create_device(IDirect3D9Ex *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                            D3DPRESENT_PARAMETERS *pp, void **out)
{
    dev_t *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    d->vtbl = v_dev; d->ref = 1;
    d->d3d = d3d;
    IDirect3D9Ex_AddRef(d->d3d);
    d->cp.AdapterOrdinal = adapter; d->cp.DeviceType = type; d->cp.hFocusWindow = focus; d->cp.BehaviorFlags = flags;
    d->pp = *pp;
    if (!d->pp.hDeviceWindow) d->pp.hDeviceWindow = focus;
    make_backbuffers(d);
    *pp = d->pp;
    d->swapchain = new_obj(v_swap, d, 0);
    emit_device_defaults(d);
    logmsg("CreateDevice %ux%u flags 0x%lx\n", d->pp.BackBufferWidth, d->pp.BackBufferHeight, flags);
    attach_window(d);
    *out = d;
    return D3D_OK;
}

/* ---------------------------------------------------------------- vtable setup */
#define SET(tbl, iface, m, fn) (tbl)[M_##iface##_##m] = (void *)(fn)

static void fill_common(void **tbl, void *const *stubs, UINT n)
{
    memcpy(tbl, stubs, n * sizeof(void *));
    tbl[0] = (void *)o_QueryInterface; tbl[1] = (void *)o_AddRef; tbl[2] = (void *)o_Release;
    tbl[3] = (void *)o_GetDevice; /* every object interface has GetDevice at slot 3 except swapchain */
}

static void resource_methods(void **t)
{
    /* IDirect3DResource9 layout prefix: SetPrivateData 4 GetPrivateData 5 FreePrivateData 6 SetPriority 7
       GetPriority 8 PreLoad 9 GetType 10 */
    t[4] = (void *)ok; t[6] = (void *)ok;
    t[7] = (void *)o_SetPriority; t[8] = (void *)o_GetPriority; t[9] = (void *)ok; t[10] = (void *)o_GetType;
}

static void basetex_methods(void **t)
{
    resource_methods(t);
    SET(t, IDirect3DBaseTexture9, SetLOD, o_SetLOD); SET(t, IDirect3DBaseTexture9, GetLOD, o_GetLOD);
    SET(t, IDirect3DBaseTexture9, GetLevelCount, o_GetLevelCount);
    SET(t, IDirect3DBaseTexture9, SetAutoGenFilterType, ok);
    SET(t, IDirect3DBaseTexture9, GetAutoGenFilterType, o_GetAutoGenFilterType);
    SET(t, IDirect3DBaseTexture9, GenerateMipSubLevels, ok);
}

static void setup_vtables(void)
{
    fill_common(v_surf, stubs_IDirect3DSurface9, M_IDirect3DSurface9_N);
    resource_methods(v_surf);
    SET(v_surf, IDirect3DSurface9, GetContainer, surf_GetContainer);
    SET(v_surf, IDirect3DSurface9, GetDesc, surf_GetDesc);
    SET(v_surf, IDirect3DSurface9, LockRect, surf_LockRect);
    SET(v_surf, IDirect3DSurface9, UnlockRect, surf_UnlockRect);

    fill_common(v_volume, stubs_IDirect3DVolume9, M_IDirect3DVolume9_N);
    SET(v_volume, IDirect3DVolume9, GetDesc, volume_GetDesc);
    SET(v_volume, IDirect3DVolume9, LockBox, volume_LockBox);
    SET(v_volume, IDirect3DVolume9, UnlockBox, volume_UnlockBox);

    fill_common(v_tex, stubs_IDirect3DTexture9, M_IDirect3DTexture9_N);
    basetex_methods(v_tex);
    SET(v_tex, IDirect3DTexture9, GetLevelDesc, tex_GetLevelDesc);
    SET(v_tex, IDirect3DTexture9, GetSurfaceLevel, tex_GetSurfaceLevel);
    SET(v_tex, IDirect3DTexture9, LockRect, tex_LockRect);
    SET(v_tex, IDirect3DTexture9, UnlockRect, tex_UnlockRect);
    SET(v_tex, IDirect3DTexture9, AddDirtyRect, ok);

    fill_common(v_cube, stubs_IDirect3DCubeTexture9, M_IDirect3DCubeTexture9_N);
    basetex_methods(v_cube);
    SET(v_cube, IDirect3DCubeTexture9, GetLevelDesc, tex_GetLevelDesc);  /* face 0 desc == any face */
    SET(v_cube, IDirect3DCubeTexture9, GetCubeMapSurface, cube_GetCubeMapSurface);
    SET(v_cube, IDirect3DCubeTexture9, LockRect, cube_LockRect);
    SET(v_cube, IDirect3DCubeTexture9, UnlockRect, cube_UnlockRect);
    SET(v_cube, IDirect3DCubeTexture9, AddDirtyRect, ok);

    fill_common(v_vol, stubs_IDirect3DVolumeTexture9, M_IDirect3DVolumeTexture9_N);
    basetex_methods(v_vol);
    SET(v_vol, IDirect3DVolumeTexture9, GetLevelDesc, vol_GetLevelDesc);
    SET(v_vol, IDirect3DVolumeTexture9, GetVolumeLevel, vol_GetVolumeLevel);
    SET(v_vol, IDirect3DVolumeTexture9, LockBox, vol_LockBox);
    SET(v_vol, IDirect3DVolumeTexture9, UnlockBox, vol_UnlockBox);
    SET(v_vol, IDirect3DVolumeTexture9, AddDirtyBox, ok);

    fill_common(v_vb, stubs_IDirect3DVertexBuffer9, M_IDirect3DVertexBuffer9_N);
    resource_methods(v_vb);
    SET(v_vb, IDirect3DVertexBuffer9, Lock, buf_Lock); SET(v_vb, IDirect3DVertexBuffer9, Unlock, ok);
    SET(v_vb, IDirect3DVertexBuffer9, GetDesc, vb_GetDesc);
    fill_common(v_ib, stubs_IDirect3DIndexBuffer9, M_IDirect3DIndexBuffer9_N);
    resource_methods(v_ib);
    SET(v_ib, IDirect3DIndexBuffer9, Lock, buf_Lock); SET(v_ib, IDirect3DIndexBuffer9, Unlock, ok);
    SET(v_ib, IDirect3DIndexBuffer9, GetDesc, ib_GetDesc);

    fill_common(v_decl, stubs_IDirect3DVertexDeclaration9, M_IDirect3DVertexDeclaration9_N);
    SET(v_decl, IDirect3DVertexDeclaration9, GetDeclaration, decl_GetDeclaration);
    fill_common(v_vs, stubs_IDirect3DVertexShader9, M_IDirect3DVertexShader9_N);
    SET(v_vs, IDirect3DVertexShader9, GetFunction, shader_GetFunction);
    fill_common(v_ps, stubs_IDirect3DPixelShader9, M_IDirect3DPixelShader9_N);
    SET(v_ps, IDirect3DPixelShader9, GetFunction, shader_GetFunction);
    fill_common(v_query, stubs_IDirect3DQuery9, M_IDirect3DQuery9_N);
    SET(v_query, IDirect3DQuery9, GetType, q_GetType); SET(v_query, IDirect3DQuery9, GetDataSize, q_GetDataSize);
    SET(v_query, IDirect3DQuery9, Issue, q_Issue); SET(v_query, IDirect3DQuery9, GetData, q_GetData);
    fill_common(v_sb, stubs_IDirect3DStateBlock9, M_IDirect3DStateBlock9_N);
    SET(v_sb, IDirect3DStateBlock9, Capture, ok); SET(v_sb, IDirect3DStateBlock9, Apply, ok);

    memcpy(v_swap, stubs_IDirect3DSwapChain9Ex, sizeof v_swap);
    v_swap[0] = (void *)o_QueryInterface; v_swap[1] = (void *)o_AddRef; v_swap[2] = (void *)o_Release;
    SET(v_swap, IDirect3DSwapChain9Ex, Present, ok);   /* TF2 presents through the device */
    SET(v_swap, IDirect3DSwapChain9Ex, GetBackBuffer, sc_GetBackBuffer);
    SET(v_swap, IDirect3DSwapChain9Ex, GetDevice, o_GetDevice);
    SET(v_swap, IDirect3DSwapChain9Ex, GetPresentParameters, sc_GetPresentParameters);

    /* device: start from loud stubs, then mark plain setters/no-ops and implement the rest */
    memcpy(v_dev, stubs_IDirect3DDevice9Ex, sizeof v_dev);
    static const int noops[] = {
        DEV_EvictManagedResources, DEV_SetCursorProperties, DEV_SetCursorPosition, DEV_SetDialogBoxMode,
        DEV_SetGammaRamp, DEV_GetFrontBufferData,
        DEV_BeginScene, DEV_EndScene, DEV_SetTransform, DEV_MultiplyTransform, DEV_SetMaterial, DEV_SetLight,
        DEV_LightEnable, DEV_BeginStateBlock, DEV_SetClipStatus, DEV_SetTextureStageState, DEV_ValidateDevice,
        DEV_SetPaletteEntries, DEV_SetCurrentTexturePalette, DEV_SetSoftwareVertexProcessing, DEV_SetNPatchMode,
        DEV_DrawPrimitiveUP, DEV_DrawIndexedPrimitiveUP, DEV_SetFVF, DEV_SetStreamSourceFreq, DEV_TestCooperativeLevel,
    };
    for (size_t i = 0; i < sizeof noops / sizeof *noops; i++) v_dev[noops[i]] = (void *)ok;
    v_dev[DEV_QueryInterface] = dev_QueryInterface; v_dev[DEV_AddRef] = dev_AddRef; v_dev[DEV_Release] = dev_Release;
    v_dev[DEV_GetAvailableTextureMem] = dev_GetAvailableTextureMem; v_dev[DEV_GetDirect3D] = dev_GetDirect3D;
    v_dev[DEV_GetDeviceCaps] = dev_GetDeviceCaps; v_dev[DEV_GetDisplayMode] = dev_GetDisplayMode;
    v_dev[DEV_GetCreationParameters] = dev_GetCreationParameters; v_dev[DEV_ShowCursor] = dev_ShowCursor;
    v_dev[DEV_GetSwapChain] = dev_GetSwapChain; v_dev[DEV_GetNumberOfSwapChains] = dev_GetNumberOfSwapChains;
    v_dev[DEV_Reset] = dev_Reset; v_dev[DEV_Present] = dev_Present; v_dev[DEV_GetBackBuffer] = dev_GetBackBuffer;
    v_dev[DEV_GetRasterStatus] = dev_GetRasterStatus; v_dev[DEV_GetGammaRamp] = dev_GetGammaRamp;
    v_dev[DEV_CreateTexture] = dev_CreateTexture; v_dev[DEV_CreateVolumeTexture] = dev_CreateVolumeTexture;
    v_dev[DEV_CreateCubeTexture] = dev_CreateCubeTexture; v_dev[DEV_CreateVertexBuffer] = dev_CreateVertexBuffer;
    v_dev[DEV_CreateIndexBuffer] = dev_CreateIndexBuffer; v_dev[DEV_CreateRenderTarget] = dev_CreateRenderTarget;
    v_dev[DEV_CreateDepthStencilSurface] = dev_CreateDepthStencilSurface;
    v_dev[DEV_CreateOffscreenPlainSurface] = dev_CreateOffscreenPlainSurface;
    v_dev[DEV_SetRenderTarget] = dev_SetRenderTarget; v_dev[DEV_GetRenderTarget] = dev_GetRenderTarget;
    v_dev[DEV_SetDepthStencilSurface] = dev_SetDepthStencilSurface;
    v_dev[DEV_GetDepthStencilSurface] = dev_GetDepthStencilSurface;
    v_dev[DEV_CreateStateBlock] = dev_CreateStateBlock; v_dev[DEV_EndStateBlock] = dev_EndStateBlock;
    v_dev[DEV_CreateVertexDeclaration] = dev_CreateVertexDeclaration;
    v_dev[DEV_CreateVertexShader] = dev_CreateVertexShader; v_dev[DEV_CreatePixelShader] = dev_CreatePixelShader;
    v_dev[DEV_CreateQuery] = dev_CreateQuery; v_dev[DEV_GetTexture] = dev_GetTexture; v_dev[DEV_GetFVF] = dev_GetFVF;
    v_dev[DEV_UpdateTexture] = dev_UpdateTexture; v_dev[DEV_UpdateSurface] = dev_UpdateSurface;
    v_dev[DEV_GetRenderTargetData] = dev_GetRenderTargetData;
#define DEVM(m) v_dev[DEV_##m] = (void *)dev_##m
    DEVM(SetRenderState); DEVM(GetRenderState); DEVM(SetSamplerState); DEVM(SetTexture); DEVM(SetStreamSource);
    DEVM(SetIndices); DEVM(SetVertexDeclaration); DEVM(SetVertexShader); DEVM(SetPixelShader);
    DEVM(SetVertexShaderConstantF); DEVM(SetPixelShaderConstantF); DEVM(SetVertexShaderConstantI);
    DEVM(SetPixelShaderConstantI); DEVM(SetVertexShaderConstantB); DEVM(SetPixelShaderConstantB);
    DEVM(SetViewport); DEVM(GetViewport); DEVM(SetScissorRect); DEVM(GetScissorRect); DEVM(SetClipPlane);
    DEVM(Clear); DEVM(DrawIndexedPrimitive); DEVM(DrawPrimitive); DEVM(StretchRect); DEVM(ColorFill);
    v_dev[DEV_GetTexture] = (void *)dev_GetTexture2;

}

/* ---------------------------------------------------------------- exports */
static BOOL init_once(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static BOOL okv;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return okv;
    char dir[MAX_PATH] = "Z:\\tmp", tag[128] = "untagged", path[MAX_PATH];
    GetEnvironmentVariableA("TF2MT_TRACE_DIR", dir, sizeof dir);
    GetEnvironmentVariableA("TF2MT_TAG", tag, sizeof tag);
    snprintf(path, sizeof path, "%s\\tf2mt-%s.log", dir, tag);
    logf_ = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)init_once, &self);
    NTSTATUS st = NtQueryVirtualMemory(GetCurrentProcess(), self, MemoryWineUnixFuncs, &unix_handle, sizeof unix_handle, NULL);
    struct tf2mt_init_params ip = {{0}};
    GetEnvironmentVariableA("TF2MT_UNIX_LOG", ip.log_path, sizeof ip.log_path);
    char v[8] = "";
    if (GetEnvironmentVariableA("TF2MT_VSYNC", v, sizeof v)) g_vsync = v[0] != '0';
    if (GetEnvironmentVariableA("TF2MT_NOPRESENT", v, sizeof v)) g_nopresent = v[0] == '1';
    NTSTATUS ist = unix_call(TF2MT_UNIX_INIT, &ip);
    QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
    logmsg("tf2mt.dll: unixlib query 0x%lx handle %llx init 0x%lx\n", st, (unsigned long long)unix_handle, ist);
    setup_vtables();
    InitializeCriticalSection(&g_cs);
    g_cmd = VirtualAlloc(NULL, CMD_CAP * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    okv = TRUE;   /* a failed unix-lib init only means nothing is presented (logged above) */
    InitOnceComplete(&once, 0, NULL);
    return okv;
}

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    if (!init_once()) return NULL;
    return (IDirect3D9 *)tf2mt_adapter_create();
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    if (!init_once()) return E_FAIL;
    *out = tf2mt_adapter_create();
    return D3D_OK;
}

__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return 0; }
__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) { return 0; }
__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {}
__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) {}
__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o) {}
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
