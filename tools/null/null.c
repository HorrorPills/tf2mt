/* tools/null — THROWAWAY Phase-0 null renderer (PLAN.md P0.3): an x86_64 PE "d3d9.dll" whose device
 * accepts every call and draws nothing. Purpose: measure TF2's game-only frame-time ceiling.
 *
 * - IDirect3D9(Ex) is the oracle's object (d3d9_oracle.dll = DXVK, black box) behind forwarding thunks,
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

/* ---------------------------------------------------------------- logging + stubs */
static HANDLE logf_ = INVALID_HANDLE_VALUE;
static void logmsg(const char *fmt, ...)
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

/* forwarding thunks for IDirect3D9Ex (shared generator with tools/trace) */
extern void *d3d_thunks[D3D_NMETHODS];
uint64_t d3d_calls[D3D_NMETHODS];
extern void *dev_thunks[DEV_NMETHODS]; /* unused here, emitted by the same generator */
uint64_t dev_calls[DEV_NMETHODS];

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
};

struct device {
    void **vtbl;
    volatile LONG ref;
    IDirect3D9Ex *d3d;         /* wrapper object handed out by Direct3DCreate9(Ex) */
    IDirect3D9Ex *oracle;      /* oracle's IDirect3D9Ex for caps */
    D3DDEVICE_CREATION_PARAMETERS cp;
    D3DPRESENT_PARAMETERS pp;
    obj_t *backbuffer, *autods, *swapchain;
    obj_t *rt[4], *ds;
};

static void *v_dev[M_IDirect3DDevice9Ex_N], *v_surf[M_IDirect3DSurface9_N], *v_tex[M_IDirect3DTexture9_N],
            *v_cube[M_IDirect3DCubeTexture9_N], *v_vol[M_IDirect3DVolumeTexture9_N], *v_volume[M_IDirect3DVolume9_N],
            *v_vb[M_IDirect3DVertexBuffer9_N], *v_ib[M_IDirect3DIndexBuffer9_N], *v_decl[M_IDirect3DVertexDeclaration9_N],
            *v_vs[M_IDirect3DVertexShader9_N], *v_ps[M_IDirect3DPixelShader9_N], *v_query[M_IDirect3DQuery9_N],
            *v_sb[M_IDirect3DStateBlock9_N], *v_swap[M_IDirect3DSwapChain9Ex_N], *v_d3d[D3D_NMETHODS];

static obj_t *new_obj(void **vtbl, struct device *dev, D3DRESOURCETYPE type)
{
    obj_t *o = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
    o->vtbl = vtbl; o->ref = 1; o->dev = dev; o->type = type;
    return o;
}

static void free_obj(obj_t *o)
{
    for (UINT i = 0; i < o->nsubs; i++) free_obj(o->subs[i]);
    if (o->subs) HeapFree(GetProcessHeap(), 0, o->subs);
    if (o->mem) VirtualFree(o->mem, 0, MEM_RELEASE);
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
    return D3D_OK;
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
            t->subs[f * t->levels + l] = s;
        }
    return t;
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
static HRESULT WINAPI buf_Lock(obj_t *b, UINT off, UINT size, void **p, DWORD flags)
{
    *p = o_mem(b) + off;
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
static HRESULT WINAPI q_GetData(obj_t *q, void *data, DWORD size, DWORD flags)
{
    if (!data || !size) return D3D_OK;
    memset(data, 0, size);
    switch (q->qtype) {
    case D3DQUERYTYPE_EVENT: *(BOOL *)data = TRUE; break;
    case D3DQUERYTYPE_OCCLUSION: *(DWORD *)data = 1000; break; /* "visible": keep flares/glows on the normal path */
    case D3DQUERYTYPE_TIMESTAMPFREQ: if (size >= 8) *(UINT64 *)data = 1000000000ull; break;
    default: break;
    }
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
    return IDirect3D9Ex_GetDeviceCaps(d->oracle, d->cp.AdapterOrdinal, d->cp.DeviceType, caps);
}
static HRESULT WINAPI dev_GetDisplayMode(dev_t *d, UINT sc, D3DDISPLAYMODE *m)
{
    return IDirect3D9Ex_GetAdapterDisplayMode(d->oracle, d->cp.AdapterOrdinal, m);
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
    d->backbuffer = new_surface(d, NULL, pp->BackBufferFormat, pp->BackBufferWidth, pp->BackBufferHeight,
                                D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, pp->MultiSampleType);
    d->autods = pp->EnableAutoDepthStencil
        ? new_surface(d, NULL, pp->AutoDepthStencilFormat, pp->BackBufferWidth, pp->BackBufferHeight,
                      D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, pp->MultiSampleType)
        : NULL;
    set_ref(&d->rt[0], d->backbuffer);
    if (d->autods) set_ref(&d->ds, d->autods);
}

static HRESULT WINAPI dev_Reset(dev_t *d, D3DPRESENT_PARAMETERS *pp)
{
    d->pp = *pp;
    make_backbuffers(d);
    *pp = d->pp;
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

static HRESULT WINAPI dev_CreateTexture(dev_t *d, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt,
                                        D3DPOOL pool, obj_t **out, HANDLE *shared)
{
    *out = new_texture(d, v_tex, D3DRTYPE_TEXTURE, w, h, 1, levels, usage, fmt, pool);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateVolumeTexture(dev_t *d, UINT w, UINT h, UINT depth, UINT levels, DWORD usage,
                                              D3DFORMAT fmt, D3DPOOL pool, obj_t **out, HANDLE *shared)
{
    *out = new_texture(d, v_vol, D3DRTYPE_VOLUMETEXTURE, w, h, depth, levels, usage, fmt, pool);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateCubeTexture(dev_t *d, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                            D3DPOOL pool, obj_t **out, HANDLE *shared)
{
    *out = new_texture(d, v_cube, D3DRTYPE_CUBETEXTURE, edge, edge, 1, levels, usage, fmt, pool);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateVertexBuffer(dev_t *d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                             obj_t **out, HANDLE *shared)
{
    obj_t *b = new_obj(v_vb, d, D3DRTYPE_VERTEXBUFFER);
    b->w = len; b->memsize = len; b->usage = usage; b->fvf = fvf; b->pool = pool;
    *out = b;
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateIndexBuffer(dev_t *d, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                            obj_t **out, HANDLE *shared)
{
    obj_t *b = new_obj(v_ib, d, D3DRTYPE_INDEXBUFFER);
    b->w = len; b->memsize = len; b->usage = usage; b->fmt = fmt; b->pool = pool;
    *out = b;
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateRenderTarget(dev_t *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                             DWORD q, BOOL lockable, obj_t **out, HANDLE *shared)
{
    *out = new_surface(d, NULL, fmt, w, h, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ms);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateDepthStencilSurface(dev_t *d, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                    DWORD q, BOOL discard, obj_t **out, HANDLE *shared)
{
    *out = new_surface(d, NULL, fmt, w, h, D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ms);
    return D3D_OK;
}
static HRESULT WINAPI dev_CreateOffscreenPlainSurface(dev_t *d, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                      obj_t **out, HANDLE *shared)
{
    *out = new_surface(d, NULL, fmt, w, h, 0, pool, D3DMULTISAMPLE_NONE);
    return D3D_OK;
}
static HRESULT WINAPI dev_SetRenderTarget(dev_t *d, DWORD i, obj_t *s)
{
    if (i >= 4) return D3DERR_INVALIDCALL;
    set_ref(&d->rt[i], s);
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
    return D3D_OK;
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
static HRESULT WINAPI dev_Present(dev_t *d, const RECT *s, const RECT *t, HWND w, const RGNDATA *r) { return D3D_OK; }
static HRESULT WINAPI dev_GetTexture(dev_t *d, DWORD stage, obj_t **out) { *out = NULL; return D3D_OK; }
static HRESULT WINAPI dev_GetFVF(dev_t *d, DWORD *fvf) { *fvf = 0; return D3D_OK; }

/* ---------------------------------------------------------------- IDirect3D9 wrapper (oracle) */
typedef struct { void **vtbl; IDirect3D9Ex *inner; } d3dwrap_t;

static ULONG WINAPI d3d_Release(d3dwrap_t *w)
{
    return IDirect3D9Ex_Release(w->inner); /* wrapper leaked on purpose (throwaway) */
}

static HRESULT create_device(d3dwrap_t *w, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                             D3DPRESENT_PARAMETERS *pp, dev_t **out)
{
    dev_t *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    d->vtbl = v_dev; d->ref = 1;
    d->d3d = (IDirect3D9Ex *)w; d->oracle = w->inner;
    IDirect3D9Ex_AddRef(d->d3d);
    d->cp.AdapterOrdinal = adapter; d->cp.DeviceType = type; d->cp.hFocusWindow = focus; d->cp.BehaviorFlags = flags;
    d->pp = *pp;
    if (!d->pp.hDeviceWindow) d->pp.hDeviceWindow = focus;
    make_backbuffers(d);
    *pp = d->pp;
    d->swapchain = new_obj(v_swap, d, 0);
    logmsg("null CreateDevice %ux%u flags 0x%lx\n", d->pp.BackBufferWidth, d->pp.BackBufferHeight, flags);
    *out = d;
    return D3D_OK;
}
static HRESULT WINAPI d3d_CreateDevice(d3dwrap_t *w, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, dev_t **out)
{
    return create_device(w, adapter, type, focus, flags, pp, out);
}
static HRESULT WINAPI d3d_CreateDeviceEx(d3dwrap_t *w, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                         D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode, dev_t **out)
{
    return create_device(w, adapter, type, focus, flags, pp, out);
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
    SET(v_surf, IDirect3DSurface9, UnlockRect, ok);

    fill_common(v_volume, stubs_IDirect3DVolume9, M_IDirect3DVolume9_N);
    SET(v_volume, IDirect3DVolume9, GetDesc, volume_GetDesc);
    SET(v_volume, IDirect3DVolume9, LockBox, volume_LockBox);
    SET(v_volume, IDirect3DVolume9, UnlockBox, ok);

    fill_common(v_tex, stubs_IDirect3DTexture9, M_IDirect3DTexture9_N);
    basetex_methods(v_tex);
    SET(v_tex, IDirect3DTexture9, GetLevelDesc, tex_GetLevelDesc);
    SET(v_tex, IDirect3DTexture9, GetSurfaceLevel, tex_GetSurfaceLevel);
    SET(v_tex, IDirect3DTexture9, LockRect, tex_LockRect);
    SET(v_tex, IDirect3DTexture9, UnlockRect, ok);
    SET(v_tex, IDirect3DTexture9, AddDirtyRect, ok);

    fill_common(v_cube, stubs_IDirect3DCubeTexture9, M_IDirect3DCubeTexture9_N);
    basetex_methods(v_cube);
    SET(v_cube, IDirect3DCubeTexture9, GetLevelDesc, tex_GetLevelDesc);  /* face 0 desc == any face */
    SET(v_cube, IDirect3DCubeTexture9, GetCubeMapSurface, cube_GetCubeMapSurface);
    SET(v_cube, IDirect3DCubeTexture9, LockRect, cube_LockRect);
    SET(v_cube, IDirect3DCubeTexture9, UnlockRect, ok);
    SET(v_cube, IDirect3DCubeTexture9, AddDirtyRect, ok);

    fill_common(v_vol, stubs_IDirect3DVolumeTexture9, M_IDirect3DVolumeTexture9_N);
    basetex_methods(v_vol);
    SET(v_vol, IDirect3DVolumeTexture9, GetLevelDesc, vol_GetLevelDesc);
    SET(v_vol, IDirect3DVolumeTexture9, GetVolumeLevel, vol_GetVolumeLevel);
    SET(v_vol, IDirect3DVolumeTexture9, LockBox, vol_LockBox);
    SET(v_vol, IDirect3DVolumeTexture9, UnlockBox, ok);
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
    SET(v_query, IDirect3DQuery9, Issue, ok); SET(v_query, IDirect3DQuery9, GetData, q_GetData);
    fill_common(v_sb, stubs_IDirect3DStateBlock9, M_IDirect3DStateBlock9_N);
    SET(v_sb, IDirect3DStateBlock9, Capture, ok); SET(v_sb, IDirect3DStateBlock9, Apply, ok);

    memcpy(v_swap, stubs_IDirect3DSwapChain9Ex, sizeof v_swap);
    v_swap[0] = (void *)o_QueryInterface; v_swap[1] = (void *)o_AddRef; v_swap[2] = (void *)o_Release;
    SET(v_swap, IDirect3DSwapChain9Ex, Present, ok);
    SET(v_swap, IDirect3DSwapChain9Ex, GetBackBuffer, sc_GetBackBuffer);
    SET(v_swap, IDirect3DSwapChain9Ex, GetDevice, o_GetDevice);
    SET(v_swap, IDirect3DSwapChain9Ex, GetPresentParameters, sc_GetPresentParameters);

    /* device: start from loud stubs, then mark plain setters/no-ops and implement the rest */
    memcpy(v_dev, stubs_IDirect3DDevice9Ex, sizeof v_dev);
    static const int noops[] = {
        DEV_EvictManagedResources, DEV_SetCursorProperties, DEV_SetCursorPosition, DEV_SetDialogBoxMode,
        DEV_SetGammaRamp, DEV_UpdateSurface, DEV_UpdateTexture, DEV_GetRenderTargetData, DEV_GetFrontBufferData,
        DEV_StretchRect, DEV_ColorFill, DEV_BeginScene, DEV_EndScene, DEV_Clear, DEV_SetTransform,
        DEV_MultiplyTransform, DEV_SetViewport, DEV_SetMaterial, DEV_SetLight, DEV_LightEnable, DEV_SetClipPlane,
        DEV_SetRenderState, DEV_BeginStateBlock, DEV_SetClipStatus, DEV_SetTexture, DEV_SetTextureStageState,
        DEV_SetSamplerState, DEV_ValidateDevice, DEV_SetPaletteEntries, DEV_SetCurrentTexturePalette,
        DEV_SetScissorRect, DEV_SetSoftwareVertexProcessing, DEV_SetNPatchMode, DEV_DrawPrimitive,
        DEV_DrawIndexedPrimitive, DEV_DrawPrimitiveUP, DEV_DrawIndexedPrimitiveUP, DEV_SetVertexDeclaration,
        DEV_SetFVF, DEV_SetVertexShader, DEV_SetVertexShaderConstantF, DEV_SetVertexShaderConstantI,
        DEV_SetVertexShaderConstantB, DEV_SetStreamSource, DEV_SetStreamSourceFreq, DEV_SetIndices,
        DEV_SetPixelShader, DEV_SetPixelShaderConstantF, DEV_SetPixelShaderConstantI, DEV_SetPixelShaderConstantB,
        DEV_TestCooperativeLevel,
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

    memcpy(v_d3d, d3d_thunks, sizeof v_d3d);
    v_d3d[D3D_Release] = d3d_Release;
    v_d3d[D3D_CreateDevice] = d3d_CreateDevice;
    v_d3d[D3D_CreateDeviceEx] = d3d_CreateDeviceEx;
}

/* ---------------------------------------------------------------- exports */
static HMODULE oracle;
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
    snprintf(path, sizeof path, "%s\\null-%s.log", dir, tag);
    logf_ = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    WCHAR wpath[MAX_PATH];
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)init_once, &self);
    DWORD n = GetModuleFileNameW(self, wpath, MAX_PATH);
    while (n && wpath[n - 1] != L'\\') n--;
    lstrcpyW(wpath + n, L"d3d9_oracle.dll");
    oracle = LoadLibraryW(wpath);
    logmsg("tf2mt null renderer; oracle %p\n", (void *)oracle);
    setup_vtables();
    okv = oracle != NULL;
    InitOnceComplete(&once, 0, NULL);
    return okv;
}

static d3dwrap_t *wrap_d3d(IDirect3D9Ex *inner)
{
    d3dwrap_t *w = HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    w->vtbl = v_d3d; w->inner = inner;
    return w;
}

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    if (!init_once()) return NULL;
    IDirect3D9 *(WINAPI *fn)(UINT) = (void *)GetProcAddress(oracle, "Direct3DCreate9");
    IDirect3D9 *d = fn(sdk);
    return d ? (IDirect3D9 *)wrap_d3d((IDirect3D9Ex *)d) : NULL;
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    if (!init_once()) return E_FAIL;
    HRESULT (WINAPI *fn)(UINT, IDirect3D9Ex **) = (void *)GetProcAddress(oracle, "Direct3DCreate9Ex");
    IDirect3D9Ex *d = NULL;
    HRESULT hr = fn(sdk, &d);
    *out = SUCCEEDED(hr) ? (IDirect3D9Ex *)wrap_d3d(d) : NULL;
    return hr;
}

__declspec(dllexport) int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return 0; }
__declspec(dllexport) int WINAPI D3DPERF_EndEvent(void) { return 0; }
__declspec(dllexport) void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) {}
__declspec(dllexport) void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) {}
__declspec(dllexport) BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return FALSE; }
__declspec(dllexport) void WINAPI D3DPERF_SetOptions(DWORD o) {}
__declspec(dllexport) DWORD WINAPI D3DPERF_GetStatus(void) { return 0; }
