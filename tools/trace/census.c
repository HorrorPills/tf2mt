/* tools/trace census mode (PLAN.md P1): records what TF2 actually uses from D3D9, as keyed counters.
 * Enabled with TF2MT_TRACE_MODE=census. Output (at device release, exit and every 3000 frames):
 *   census-<tag>.tsv         key<TAB>count   (keys are "category|detail" strings; see tools/census/report.py)
 *   census-<tag>.threads.tsv tid<TAB>method<TAB>count
 *   corpus/<hash>.{vs,ps}.bin  shader bytecode (content-addressed, FNV-1a 64)
 *   caps-<tag>.bin           raw D3DCAPS9 as returned to the game
 * Device/IDirect3D9 calls are intercepted through our wrapper vtables; resource-object calls (Lock, LockRect,
 * GetData…) by patching the oracle's (DXVK's) resource vtables in-process. The game is never touched.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include "methods.h"
#include "ifaces.h"
#include "trace_common.h"

/* ---------------------------------------------------------------- keyed counters */
#define MAP_BITS 18
#define MAP_SIZE (1u << MAP_BITS)
typedef struct { char *key; uint64_t count; } entry_t;
static entry_t *map;
static CRITICAL_SECTION cz_lock;
static int census_on;
static char out_dir[MAX_PATH], tag[128], corpus_dir[MAX_PATH];

static uint64_t fnv(const void *p, size_t n)
{
    const uint8_t *b = p; uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static void cz_add(const char *key, uint64_t n)
{
    uint64_t h = fnv(key, strlen(key));
    EnterCriticalSection(&cz_lock);
    for (uint32_t i = (uint32_t)h & (MAP_SIZE - 1);; i = (i + 1) & (MAP_SIZE - 1)) {
        if (!map[i].key) {
            size_t l = strlen(key) + 1;
            map[i].key = HeapAlloc(GetProcessHeap(), 0, l); memcpy(map[i].key, key, l);
            map[i].count = n; break;
        }
        if (!strcmp(map[i].key, key)) { map[i].count += n; break; }
    }
    LeaveCriticalSection(&cz_lock);
}

static void cz(const char *fmt, ...)
{
    char k[512];
    va_list ap; va_start(ap, fmt); vsnprintf(k, sizeof k, fmt, ap); va_end(ap);
    cz_add(k, 1);
}

/* per-thread method counts (filled by census thunks) */
typedef struct tstat { DWORD tid; uint64_t n[2][DEV_NMETHODS > D3D_NMETHODS ? DEV_NMETHODS : D3D_NMETHODS]; struct tstat *next; } tstat_t;
static tstat_t *threads;
static DWORD tls_slot = TLS_OUT_OF_INDEXES;

void census_hit(DWORD code)
{
    if (capture_on) { capture_hit(code); return; }
    tstat_t *t = TlsGetValue(tls_slot);
    if (!t) {
        t = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *t);
        t->tid = GetCurrentThreadId();
        EnterCriticalSection(&cz_lock); t->next = threads; threads = t; LeaveCriticalSection(&cz_lock);
        TlsSetValue(tls_slot, t);
    }
    t->n[code >> 16][code & 0xffff]++;
}

static void write_file(const char *path, const void *data, DWORD n)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD w;
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, data, n, &w, NULL); CloseHandle(h); }
}

void census_dump(void)
{
    if (!census_on) return;
    static char buf[8 << 20];
    size_t len = 0;
    EnterCriticalSection(&cz_lock);
    for (uint32_t i = 0; i < MAP_SIZE && len < sizeof buf - 600; i++)
        if (map[i].key) len += snprintf(buf + len, sizeof buf - len, "%s\t%llu\n", map[i].key, (unsigned long long)map[i].count);
    LeaveCriticalSection(&cz_lock);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\census-%s.tsv", out_dir, tag);
    write_file(path, buf, (DWORD)len);
    len = 0;
    EnterCriticalSection(&cz_lock);
    for (tstat_t *t = threads; t; t = t->next) {
        for (int i = 0; i < DEV_NMETHODS; i++)
            if (t->n[1][i]) len += snprintf(buf + len, sizeof buf - len, "%lu\tdev.%s\t%llu\n", t->tid, dev_method_names[i], (unsigned long long)t->n[1][i]);
        for (int i = 0; i < D3D_NMETHODS; i++)
            if (t->n[0][i]) len += snprintf(buf + len, sizeof buf - len, "%lu\td3d.%s\t%llu\n", t->tid, d3d_method_names[i], (unsigned long long)t->n[0][i]);
    }
    LeaveCriticalSection(&cz_lock);
    snprintf(path, sizeof path, "%s\\census-%s.threads.tsv", out_dir, tag);
    write_file(path, buf, (DWORD)len);
}

/* ---------------------------------------------------------------- helpers */
static const char *pool_s(D3DPOOL p) { return p == D3DPOOL_DEFAULT ? "default" : p == D3DPOOL_MANAGED ? "managed" : p == D3DPOOL_SYSTEMMEM ? "sysmem" : p == D3DPOOL_SCRATCH ? "scratch" : "?"; }
static const char *fmt_s(D3DFORMAT f, char *b)
{
    if ((DWORD)f > 0xff) { /* FOURCC */
        b[0] = (char)(f & 0xff); b[1] = (char)((f >> 8) & 0xff); b[2] = (char)((f >> 16) & 0xff); b[3] = (char)(f >> 24); b[4] = 0;
        for (int i = 0; i < 4; i++) if (b[i] < 32 || b[i] > 126) b[i] = '?';
        return b;
    }
    snprintf(b, 16, "%u", (unsigned)f);
    return b;
}
static unsigned pow2_bucket(unsigned v) { unsigned b = 1; while (b < v) b <<= 1; return b; }

static void surface_fmt(IDirect3DSurface9 *s, char *b)
{
    D3DSURFACE_DESC d;
    if (s && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &d))) {
        char f[16];
        snprintf(b, 64, "%s ms%u%s", fmt_s(d.Format, f), d.MultiSampleType, d.Usage & D3DUSAGE_DEPTHSTENCIL ? " ds" : d.Usage & D3DUSAGE_RENDERTARGET ? " rt" : "");
    } else strcpy(b, "null");
}

/* ---------------------------------------------------------------- resource vtable patches (oracle objects) */
typedef struct { void **vtbl; } com_t;
#define MAXPATCH 64
static void **patched[MAXPATCH]; static int npatched;

static void *patch_slot(void **vtbl, int idx, void *hook)
{
    for (int i = 0; i < npatched; i++) if (patched[i] == vtbl + idx) return NULL;   /* already patched */
    if (npatched == MAXPATCH) return NULL;
    DWORD old;
    void *orig = vtbl[idx];
    if (orig == hook) return NULL;
    VirtualProtect(&vtbl[idx], sizeof(void *), PAGE_READWRITE, &old);
    vtbl[idx] = hook;
    VirtualProtect(&vtbl[idx], sizeof(void *), old, &old);
    patched[npatched++] = vtbl + idx;
    return orig;
}
#define PATCH(obj, iface, method, hook, orig) do { \
        void *o_ = patch_slot(((com_t *)(obj))->vtbl, M_##iface##_##method, (void *)hook); \
        if (o_) orig = o_; } while (0)

static HRESULT (WINAPI *o_vb_lock)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD);
static HRESULT (WINAPI *o_ib_lock)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD);
static HRESULT (WINAPI *o_tex_lock)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
static HRESULT (WINAPI *o_cube_lock)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD);
static HRESULT (WINAPI *o_vol_lock)(IDirect3DVolumeTexture9 *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD);
static HRESULT (WINAPI *o_surf_lock)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD);
static HRESULT (WINAPI *o_surf_getdc)(IDirect3DSurface9 *, HDC *);
static HRESULT (WINAPI *o_tex_gensub)(IDirect3DTexture9 *);
static HRESULT (WINAPI *o_q_issue)(IDirect3DQuery9 *, DWORD);
static HRESULT (WINAPI *o_q_getdata)(IDirect3DQuery9 *, void *, DWORD, DWORD);

static const char *lockflags_s(DWORD f, char *b)
{
    b[0] = 0;
    if (f & D3DLOCK_DISCARD) strcat(b, "DISCARD ");
    if (f & D3DLOCK_NOOVERWRITE) strcat(b, "NOOVERWRITE ");
    if (f & D3DLOCK_READONLY) strcat(b, "READONLY ");
    if (f & D3DLOCK_NOSYSLOCK) strcat(b, "NOSYSLOCK ");
    if (f & D3DLOCK_DONOTWAIT) strcat(b, "DONOTWAIT ");
    if (f & D3DLOCK_NO_DIRTY_UPDATE) strcat(b, "NO_DIRTY_UPDATE ");
    if (!b[0]) strcpy(b, "0");
    return b;
}

static HRESULT WINAPI h_vb_lock(IDirect3DVertexBuffer9 *b, UINT off, UINT size, void **p, DWORD f)
{
    D3DVERTEXBUFFER_DESC d; char fl[96];
    IDirect3DVertexBuffer9_GetDesc(b, &d);
    cz("lock.vb|usage=0x%lx pool=%s flags=%s", d.Usage, pool_s(d.Pool), lockflags_s(f, fl));
    cz("lock.vb.size|%s bytes<=%u", (d.Usage & D3DUSAGE_DYNAMIC) ? "dynamic" : "static", pow2_bucket(size ? size : d.Size));
    return o_vb_lock(b, off, size, p, f);
}
static HRESULT WINAPI h_ib_lock(IDirect3DIndexBuffer9 *b, UINT off, UINT size, void **p, DWORD f)
{
    D3DINDEXBUFFER_DESC d; char fl[96], fb[16];
    IDirect3DIndexBuffer9_GetDesc(b, &d);
    cz("lock.ib|usage=0x%lx pool=%s fmt=%s flags=%s", d.Usage, pool_s(d.Pool), fmt_s(d.Format, fb), lockflags_s(f, fl));
    cz("lock.ib.size|%s bytes<=%u", (d.Usage & D3DUSAGE_DYNAMIC) ? "dynamic" : "static", pow2_bucket(size ? size : d.Size));
    return o_ib_lock(b, off, size, p, f);
}
static HRESULT WINAPI h_tex_lock(IDirect3DTexture9 *t, UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    D3DSURFACE_DESC d; char fl[96], fb[16];
    IDirect3DTexture9_GetLevelDesc(t, l, &d);
    cz("lock.tex2d|fmt=%s usage=0x%lx pool=%s flags=%s rect=%d level=%s", fmt_s(d.Format, fb), d.Usage, pool_s(d.Pool),
       lockflags_s(f, fl), r != NULL, l ? ">0" : "0");
    return o_tex_lock(t, l, lr, r, f);
}
static HRESULT WINAPI h_cube_lock(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT l, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    D3DSURFACE_DESC d; char fl[96], fb[16];
    IDirect3DCubeTexture9_GetLevelDesc(t, l, &d);
    cz("lock.cube|fmt=%s pool=%s flags=%s", fmt_s(d.Format, fb), pool_s(d.Pool), lockflags_s(f, fl));
    return o_cube_lock(t, face, l, lr, r, f);
}
static HRESULT WINAPI h_vol_lock(IDirect3DVolumeTexture9 *t, UINT l, D3DLOCKED_BOX *lb, const D3DBOX *b, DWORD f)
{
    D3DVOLUME_DESC d; char fl[96], fb[16];
    IDirect3DVolumeTexture9_GetLevelDesc(t, l, &d);
    cz("lock.volume|fmt=%s pool=%s flags=%s %ux%ux%u", fmt_s(d.Format, fb), pool_s(d.Pool), lockflags_s(f, fl), d.Width, d.Height, d.Depth);
    return o_vol_lock(t, l, lb, b, f);
}
static HRESULT WINAPI h_surf_lock(IDirect3DSurface9 *s, D3DLOCKED_RECT *lr, const RECT *r, DWORD f)
{
    D3DSURFACE_DESC d; char fl[96], fb[16];
    IDirect3DSurface9_GetDesc(s, &d);
    cz("lock.surface|fmt=%s usage=0x%lx pool=%s flags=%s rect=%d", fmt_s(d.Format, fb), d.Usage, pool_s(d.Pool), lockflags_s(f, fl), r != NULL);
    return o_surf_lock(s, lr, r, f);
}
static HRESULT WINAPI h_surf_getdc(IDirect3DSurface9 *s, HDC *dc)
{
    cz("surface.GetDC|used");
    return o_surf_getdc(s, dc);
}
static HRESULT WINAPI h_tex_gensub(IDirect3DTexture9 *t)
{
    cz("tex.GenerateMipSubLevels|used");
    return o_tex_gensub(t);
}
static HRESULT WINAPI h_q_issue(IDirect3DQuery9 *q, DWORD f)
{
    cz("query.issue|type=%d flags=%s", IDirect3DQuery9_GetType(q), f & D3DISSUE_BEGIN ? "BEGIN" : f & D3DISSUE_END ? "END" : "0");
    return o_q_issue(q, f);
}
static HRESULT WINAPI h_q_getdata(IDirect3DQuery9 *q, void *data, DWORD size, DWORD f)
{
    HRESULT hr = o_q_getdata(q, data, size, f);
    cz("query.getdata|type=%d flush=%d size=%lu result=%s", IDirect3DQuery9_GetType(q), !!(f & D3DGETDATA_FLUSH), size,
       hr == S_OK ? "S_OK" : hr == S_FALSE ? "S_FALSE" : "error");
    return hr;
}

static void patch_surface(IDirect3DSurface9 *s)
{
    if (!s) return;
    PATCH(s, IDirect3DSurface9, LockRect, h_surf_lock, o_surf_lock);
    PATCH(s, IDirect3DSurface9, GetDC, h_surf_getdc, o_surf_getdc);
}

/* ---------------------------------------------------------------- device hooks */
#define FWD(m, ...) IDirect3DDevice9Ex_##m(INNER(IDirect3DDevice9Ex, self), __VA_ARGS__)
#define HIT(m) (dev_calls[DEV_##m]++, census_hit((1u << 16) | DEV_##m))   /* keep the per-frame counters in trace.c right */

static HRESULT WINAPI c_CreateTexture(IDirect3DDevice9Ex *self, UINT w, UINT h, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                      IDirect3DTexture9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateTexture);
    HRESULT hr = FWD(CreateTexture, w, h, lv, usage, fmt, pool, out, sh);
    cz("create.tex2d|fmt=%s usage=0x%lx pool=%s levels=%s hr=%s", fmt_s(fmt, fb), usage, pool_s(pool), lv == 1 ? "1" : lv ? "n" : "full", SUCCEEDED(hr) ? "ok" : "FAIL");
    cz("create.tex2d.size|max_dim<=%u npot=%d", pow2_bucket(w > h ? w : h), (w & (w - 1)) || (h & (h - 1)));
    if (SUCCEEDED(hr) && *out) {
        PATCH(*out, IDirect3DTexture9, LockRect, h_tex_lock, o_tex_lock);
        PATCH(*out, IDirect3DTexture9, GenerateMipSubLevels, h_tex_gensub, o_tex_gensub);
        if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
            IDirect3DSurface9 *s;
            if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(*out, 0, &s))) { patch_surface(s); IDirect3DSurface9_Release(s); }
        }
    }
    return hr;
}
static HRESULT WINAPI c_CreateVolumeTexture(IDirect3DDevice9Ex *self, UINT w, UINT h, UINT d, UINT lv, DWORD usage, D3DFORMAT fmt,
                                            D3DPOOL pool, IDirect3DVolumeTexture9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateVolumeTexture);
    HRESULT hr = FWD(CreateVolumeTexture, w, h, d, lv, usage, fmt, pool, out, sh);
    cz("create.volume|fmt=%s usage=0x%lx pool=%s %ux%ux%u levels=%u", fmt_s(fmt, fb), usage, pool_s(pool), w, h, d, lv);
    if (SUCCEEDED(hr) && *out) PATCH(*out, IDirect3DVolumeTexture9, LockBox, h_vol_lock, o_vol_lock);
    return hr;
}
static HRESULT WINAPI c_CreateCubeTexture(IDirect3DDevice9Ex *self, UINT e, UINT lv, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                          IDirect3DCubeTexture9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateCubeTexture);
    HRESULT hr = FWD(CreateCubeTexture, e, lv, usage, fmt, pool, out, sh);
    cz("create.cube|fmt=%s usage=0x%lx pool=%s edge<=%u", fmt_s(fmt, fb), usage, pool_s(pool), pow2_bucket(e));
    if (SUCCEEDED(hr) && *out) PATCH(*out, IDirect3DCubeTexture9, LockRect, h_cube_lock, o_cube_lock);
    return hr;
}
static HRESULT WINAPI c_CreateVertexBuffer(IDirect3DDevice9Ex *self, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                           IDirect3DVertexBuffer9 **out, HANDLE *sh)
{
    HIT(CreateVertexBuffer);
    HRESULT hr = FWD(CreateVertexBuffer, len, usage, fvf, pool, out, sh);
    cz("create.vb|usage=0x%lx fvf=0x%lx pool=%s", usage, fvf, pool_s(pool));
    cz("create.vb.size|%s bytes<=%u", usage & D3DUSAGE_DYNAMIC ? "dynamic" : "static", pow2_bucket(len));
    if (SUCCEEDED(hr) && *out) PATCH(*out, IDirect3DVertexBuffer9, Lock, h_vb_lock, o_vb_lock);
    return hr;
}
static HRESULT WINAPI c_CreateIndexBuffer(IDirect3DDevice9Ex *self, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                          IDirect3DIndexBuffer9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateIndexBuffer);
    HRESULT hr = FWD(CreateIndexBuffer, len, usage, fmt, pool, out, sh);
    cz("create.ib|usage=0x%lx fmt=%s pool=%s", usage, fmt_s(fmt, fb), pool_s(pool));
    cz("create.ib.size|%s bytes<=%u", usage & D3DUSAGE_DYNAMIC ? "dynamic" : "static", pow2_bucket(len));
    if (SUCCEEDED(hr) && *out) PATCH(*out, IDirect3DIndexBuffer9, Lock, h_ib_lock, o_ib_lock);
    return hr;
}
static HRESULT WINAPI c_CreateRenderTarget(IDirect3DDevice9Ex *self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q,
                                           BOOL lockable, IDirect3DSurface9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateRenderTarget);
    HRESULT hr = FWD(CreateRenderTarget, w, h, fmt, ms, q, lockable, out, sh);
    cz("create.rt_surface|fmt=%s ms=%u q=%lu lockable=%d", fmt_s(fmt, fb), ms, q, lockable);
    if (SUCCEEDED(hr) && *out) patch_surface(*out);
    return hr;
}
static HRESULT WINAPI c_CreateDepthStencilSurface(IDirect3DDevice9Ex *self, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms,
                                                  DWORD q, BOOL discard, IDirect3DSurface9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateDepthStencilSurface);
    HRESULT hr = FWD(CreateDepthStencilSurface, w, h, fmt, ms, q, discard, out, sh);
    cz("create.ds_surface|fmt=%s ms=%u discard=%d", fmt_s(fmt, fb), ms, discard);
    if (SUCCEEDED(hr) && *out) patch_surface(*out);
    return hr;
}
static HRESULT WINAPI c_CreateOffscreenPlainSurface(IDirect3DDevice9Ex *self, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DSurface9 **out, HANDLE *sh)
{
    char fb[16]; HIT(CreateOffscreenPlainSurface);
    HRESULT hr = FWD(CreateOffscreenPlainSurface, w, h, fmt, pool, out, sh);
    cz("create.offscreen|fmt=%s pool=%s", fmt_s(fmt, fb), pool_s(pool));
    if (SUCCEEDED(hr) && *out) patch_surface(*out);
    return hr;
}
static HRESULT WINAPI c_CreateQuery(IDirect3DDevice9Ex *self, D3DQUERYTYPE t, IDirect3DQuery9 **out)
{
    HIT(CreateQuery);
    HRESULT hr = FWD(CreateQuery, t, out);
    cz("create.query|type=%d probe=%d hr=%s", t, out == NULL, SUCCEEDED(hr) ? "ok" : "FAIL");
    if (SUCCEEDED(hr) && out && *out) {
        PATCH(*out, IDirect3DQuery9, Issue, h_q_issue, o_q_issue);
        PATCH(*out, IDirect3DQuery9, GetData, h_q_getdata, o_q_getdata);
    }
    return hr;
}

static UINT shader_len(const DWORD *code)
{
    UINT n = 1;
    while (code[n] != 0x0000ffff) n += ((code[n] & 0xffff) == 0xfffe) ? 1 + (code[n] >> 16) : 1;
    return n + 1;
}
static void save_shader(const DWORD *code, const char *kind)
{
    UINT n = shader_len(code);
    uint64_t h = fnv(code, n * 4);
    cz("shader.%s|version=%lu.%lu", kind, (code[0] >> 8) & 0xff, code[0] & 0xff);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%016llx.%s.bin", corpus_dir, (unsigned long long)h, kind);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) { write_file(path, code, n * 4); cz("shader.%s.unique|count", kind); }
    /* opcode histogram (instruction tokens: opcode in low 16 bits, length in bits 24..27 for SM2+) */
    for (UINT i = 1; i < n - 1;) {
        DWORD tok = code[i];
        DWORD op = tok & 0xffff;
        if (op == 0xfffe) { i += 1 + (tok >> 16); continue; }
        cz("shader.%s.op|%lu", kind, op);
        if (tok & 0x40000000) cz("shader.%s.mod|coissue", kind);
        if (tok & 0x10000000) cz("shader.%s.mod|predicated", kind);
        i += 1 + ((tok >> 24) & 0xf);
    }
}
static HRESULT WINAPI c_CreateVertexShader(IDirect3DDevice9Ex *self, const DWORD *code, IDirect3DVertexShader9 **out)
{
    HIT(CreateVertexShader); save_shader(code, "vs");
    return FWD(CreateVertexShader, code, out);
}
static HRESULT WINAPI c_CreatePixelShader(IDirect3DDevice9Ex *self, const DWORD *code, IDirect3DPixelShader9 **out)
{
    HIT(CreatePixelShader); save_shader(code, "ps");
    return FWD(CreatePixelShader, code, out);
}
static HRESULT WINAPI c_CreateVertexDeclaration(IDirect3DDevice9Ex *self, const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **out)
{
    HIT(CreateVertexDeclaration);
    int streams = 0;
    for (const D3DVERTEXELEMENT9 *p = e; p->Stream != 0xff; p++) {
        cz("decl.element|type=%u method=%u usage=%u", p->Type, p->Method, p->Usage);
        cz("decl.usage|usage=%u index=%u", p->Usage, p->UsageIndex);
        if (p->Stream + 1 > streams) streams = p->Stream + 1;
    }
    cz("decl.streams|%d", streams);
    return FWD(CreateVertexDeclaration, e, out);
}
static HRESULT WINAPI c_SetRenderState(IDirect3DDevice9Ex *self, D3DRENDERSTATETYPE s, DWORD v)
{
    HIT(SetRenderState);
    cz("rs|%u=0x%lx", s, v);
    return FWD(SetRenderState, s, v);
}
static HRESULT WINAPI c_SetSamplerState(IDirect3DDevice9Ex *self, DWORD smp, D3DSAMPLERSTATETYPE t, DWORD v)
{
    HIT(SetSamplerState);
    if (t == D3DSAMP_MIPMAPLODBIAS) {              /* float bits; TF2 varies it continuously — bucket to 0.25 */
        float f; memcpy(&f, &v, 4);
        cz("ss|%u=%.2f", t, (double)((int)(f * 4.0f + (f < 0 ? -0.5f : 0.5f))) / 4.0);
    } else cz("ss|%u=0x%lx", t, v);
    cz("ss.sampler|%lu", smp);
    return FWD(SetSamplerState, smp, t, v);
}
static HRESULT WINAPI c_SetTextureStageState(IDirect3DDevice9Ex *self, DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    HIT(SetTextureStageState);
    cz("tss|stage%lu %u=0x%lx", st, t, v);
    return FWD(SetTextureStageState, st, t, v);
}
static HRESULT WINAPI c_SetTexture(IDirect3DDevice9Ex *self, DWORD st, IDirect3DBaseTexture9 *t)
{
    HIT(SetTexture);
    cz("settexture|stage=%lu type=%d", st, t ? (int)IDirect3DBaseTexture9_GetType(t) : 0);
    return FWD(SetTexture, st, t);
}
static HRESULT WINAPI c_DrawPrimitive(IDirect3DDevice9Ex *self, D3DPRIMITIVETYPE t, UINT start, UINT count)
{
    HIT(DrawPrimitive); cz("draw|DrawPrimitive prim=%d", t);
    return FWD(DrawPrimitive, t, start, count);
}
static HRESULT WINAPI c_DrawIndexedPrimitive(IDirect3DDevice9Ex *self, D3DPRIMITIVETYPE t, INT base, UINT minidx, UINT nv, UINT start, UINT pc)
{
    HIT(DrawIndexedPrimitive);
    cz("draw|DrawIndexedPrimitive prim=%d basevertex=%s", t, base ? "nonzero" : "0");
    cz("draw.prims|<=%u", pow2_bucket(pc));
    return FWD(DrawIndexedPrimitive, t, base, minidx, nv, start, pc);
}
static HRESULT WINAPI c_DrawPrimitiveUP(IDirect3DDevice9Ex *self, D3DPRIMITIVETYPE t, UINT pc, const void *d, UINT stride)
{
    HIT(DrawPrimitiveUP); cz("draw|DrawPrimitiveUP prim=%d stride=%u", t, stride);
    return FWD(DrawPrimitiveUP, t, pc, d, stride);
}
static HRESULT WINAPI c_DrawIndexedPrimitiveUP(IDirect3DDevice9Ex *self, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *idx,
                                               D3DFORMAT ifmt, const void *vd, UINT stride)
{
    char fb[16]; HIT(DrawIndexedPrimitiveUP);
    cz("draw|DrawIndexedPrimitiveUP prim=%d ifmt=%s stride=%u", t, fmt_s(ifmt, fb), stride);
    return FWD(DrawIndexedPrimitiveUP, t, mi, nv, pc, idx, ifmt, vd, stride);
}
static HRESULT WINAPI c_SetStreamSource(IDirect3DDevice9Ex *self, UINT s, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{
    HIT(SetStreamSource);
    cz("stream|index=%u stride=%u offset=%s null=%d", s, stride, off ? "nonzero" : "0", vb == NULL);
    return FWD(SetStreamSource, s, vb, off, stride);
}
static HRESULT WINAPI c_SetStreamSourceFreq(IDirect3DDevice9Ex *self, UINT s, UINT setting)
{
    HIT(SetStreamSourceFreq);
    cz("streamfreq|index=%u setting=0x%x", s, setting);
    return FWD(SetStreamSourceFreq, s, setting);
}
static HRESULT WINAPI c_Clear(IDirect3DDevice9Ex *self, DWORD n, const D3DRECT *r, DWORD flags, D3DCOLOR c, float z, DWORD st)
{
    HIT(Clear);
    cz("clear|flags=0x%lx rects=%s z=%s", flags, n ? (n == 1 ? "1" : "many") : "0", z == 1.0f ? "1" : z == 0.0f ? "0" : "other");
    return FWD(Clear, n, r, flags, c, z, st);
}
static HRESULT WINAPI c_StretchRect(IDirect3DDevice9Ex *self, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *d,
                                    const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
    char a[64], b[64]; HIT(StretchRect);
    surface_fmt(s, a); surface_fmt(d, b);
    D3DSURFACE_DESC sd, dd;
    int scaled = 0;
    if (s && d && SUCCEEDED(IDirect3DSurface9_GetDesc(s, &sd)) && SUCCEEDED(IDirect3DSurface9_GetDesc(d, &dd))) {
        LONG sw = sr ? sr->right - sr->left : (LONG)sd.Width, sh = sr ? sr->bottom - sr->top : (LONG)sd.Height;
        LONG dw = dr ? dr->right - dr->left : (LONG)dd.Width, dh = dr ? dr->bottom - dr->top : (LONG)dd.Height;
        scaled = sw != dw || sh != dh;
    }
    cz("stretchrect|%s -> %s filter=%d scaled=%d rects=%d%d", a, b, f, scaled, sr != NULL, dr != NULL);
    return FWD(StretchRect, s, sr, d, dr, f);
}
static HRESULT WINAPI c_GetRenderTargetData(IDirect3DDevice9Ex *self, IDirect3DSurface9 *rt, IDirect3DSurface9 *dst)
{
    char a[64], b[64]; HIT(GetRenderTargetData);
    surface_fmt(rt, a); surface_fmt(dst, b); patch_surface(dst);
    cz("readback|GetRenderTargetData %s -> %s", a, b);
    return FWD(GetRenderTargetData, rt, dst);
}
static HRESULT WINAPI c_UpdateSurface(IDirect3DDevice9Ex *self, IDirect3DSurface9 *s, const RECT *r, IDirect3DSurface9 *d, const POINT *p)
{
    char a[64]; HIT(UpdateSurface);
    surface_fmt(s, a); cz("upload|UpdateSurface %s rect=%d", a, r != NULL);
    return FWD(UpdateSurface, s, r, d, p);
}
static HRESULT WINAPI c_UpdateTexture(IDirect3DDevice9Ex *self, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *d)
{
    HIT(UpdateTexture);
    cz("upload|UpdateTexture type=%d", s ? (int)IDirect3DBaseTexture9_GetType(s) : 0);
    return FWD(UpdateTexture, s, d);
}
static HRESULT WINAPI c_ColorFill(IDirect3DDevice9Ex *self, IDirect3DSurface9 *s, const RECT *r, D3DCOLOR c)
{
    char a[64]; HIT(ColorFill);
    surface_fmt(s, a); cz("colorfill|%s rect=%d", a, r != NULL);
    return FWD(ColorFill, s, r, c);
}
static HRESULT WINAPI c_SetRenderTarget(IDirect3DDevice9Ex *self, DWORD i, IDirect3DSurface9 *s)
{
    char a[64]; HIT(SetRenderTarget);
    surface_fmt(s, a); cz("rt|index=%lu %s", i, a);
    return FWD(SetRenderTarget, i, s);
}
static HRESULT WINAPI c_SetDepthStencilSurface(IDirect3DDevice9Ex *self, IDirect3DSurface9 *s)
{
    char a[64]; HIT(SetDepthStencilSurface);
    surface_fmt(s, a); cz("ds|%s", a);
    return FWD(SetDepthStencilSurface, s);
}
static HRESULT WINAPI c_SetViewport(IDirect3DDevice9Ex *self, const D3DVIEWPORT9 *v)
{
    HIT(SetViewport);
    cz("viewport|minz=%.3f maxz=%.3f", v->MinZ, v->MaxZ);
    return FWD(SetViewport, v);
}
static HRESULT WINAPI c_SetScissorRect(IDirect3DDevice9Ex *self, const RECT *r)
{
    HIT(SetScissorRect); cz("scissor|set");
    return FWD(SetScissorRect, r);
}
static HRESULT WINAPI c_SetClipPlane(IDirect3DDevice9Ex *self, DWORD i, const float *p)
{
    HIT(SetClipPlane); cz("clipplane|index=%lu", i);
    return FWD(SetClipPlane, i, p);
}
static HRESULT WINAPI c_SetFVF(IDirect3DDevice9Ex *self, DWORD fvf)
{
    HIT(SetFVF); cz("fvf|0x%lx", fvf);
    return FWD(SetFVF, fvf);
}
#define CONSTS(name, T)                                                                                     \
    static HRESULT WINAPI c_##name(IDirect3DDevice9Ex *self, UINT start, const T *d, UINT n)              \
    {                                                                                                     \
        HIT(name); cz("consts|" #name " end<=%u", pow2_bucket(start + n)); cz("consts.max|" #name " %u", start + n); \
        return FWD(name, start, d, n);                                                                    \
    }
CONSTS(SetVertexShaderConstantF, float)
CONSTS(SetVertexShaderConstantI, int)
CONSTS(SetVertexShaderConstantB, BOOL)
CONSTS(SetPixelShaderConstantF, float)
CONSTS(SetPixelShaderConstantI, int)
CONSTS(SetPixelShaderConstantB, BOOL)
static HRESULT WINAPI c_CreateStateBlock(IDirect3DDevice9Ex *self, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9 **out)
{
    HIT(CreateStateBlock); cz("stateblock|create type=%d", t);
    return FWD(CreateStateBlock, t, out);
}
static HRESULT WINAPI c_GetDeviceCaps(IDirect3DDevice9Ex *self, D3DCAPS9 *caps)
{
    HIT(GetDeviceCaps);
    HRESULT hr = FWD(GetDeviceCaps, caps);
    static int once;
    if (SUCCEEDED(hr) && !once++) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\caps-%s.bin", out_dir, tag); write_file(p, caps, sizeof *caps); }
    return hr;
}

/* ---------------------------------------------------------------- IDirect3D9 hooks */
#define DFWD(m, ...) IDirect3D9Ex_##m(INNER(IDirect3D9Ex, self), __VA_ARGS__)
#define DHIT(m) census_hit(D3D_##m)
static HRESULT WINAPI c_CheckDeviceFormat(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, D3DFORMAT afmt, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT cf)
{
    char fb[16]; DHIT(CheckDeviceFormat);
    HRESULT hr = DFWD(CheckDeviceFormat, a, t, afmt, usage, rt, cf);
    char ab[16];
    cz("checkformat|fmt=%s rtype=%d usage=0x%lx adapterfmt=%s -> %s", fmt_s(cf, fb), rt, usage, fmt_s(afmt, ab), hr == D3D_OK ? "OK" : "NO");
    return hr;
}
static HRESULT WINAPI c_CheckDeviceMultiSampleType(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, D3DFORMAT f, BOOL win, D3DMULTISAMPLE_TYPE ms, DWORD *q)
{
    char fb[16]; DHIT(CheckDeviceMultiSampleType);
    HRESULT hr = DFWD(CheckDeviceMultiSampleType, a, t, f, win, ms, q);
    cz("checkms|fmt=%s ms=%u -> %s", fmt_s(f, fb), ms, hr == D3D_OK ? "OK" : "NO");
    return hr;
}
static HRESULT WINAPI c_CheckDepthStencilMatch(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df)
{
    char b1[16], b2[16]; DHIT(CheckDepthStencilMatch);
    HRESULT hr = DFWD(CheckDepthStencilMatch, a, t, af, rf, df);
    cz("checkdsmatch|rt=%s ds=%s -> %s", fmt_s(rf, b1), fmt_s(df, b2), hr == D3D_OK ? "OK" : "NO");
    return hr;
}
static HRESULT WINAPI c_CheckDeviceFormatConversion(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d)
{
    char b1[16], b2[16]; DHIT(CheckDeviceFormatConversion);
    HRESULT hr = DFWD(CheckDeviceFormatConversion, a, t, s, d);
    cz("checkconversion|%s -> %s -> %s", fmt_s(s, b1), fmt_s(d, b2), hr == D3D_OK ? "OK" : "NO");
    return hr;
}

static HRESULT WINAPI c_CheckDeviceType(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, BOOL windowed)
{
    char b1[16], b2[16]; DHIT(CheckDeviceType);
    HRESULT hr = DFWD(CheckDeviceType, a, t, af, bf, windowed);
    cz("checktype|adapterfmt=%s backbuffer=%s windowed=%d devtype=%d -> %s", fmt_s(af, b1), fmt_s(bf, b2), windowed, t, hr == D3D_OK ? "OK" : "NO");
    return hr;
}
static HRESULT WINAPI c_GetAdapterModeCount_(IDirect3D9Ex *self, UINT a, D3DFORMAT f)
{
    char fb[16]; DHIT(GetAdapterModeCount);
    UINT n = DFWD(GetAdapterModeCount, a, f);
    cz("modecount|fmt=%s -> %u", fmt_s(f, fb), n);
    return n;
}
static HRESULT WINAPI c_EnumAdapterModes(IDirect3D9Ex *self, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE *m)
{
    char fb[16]; DHIT(EnumAdapterModes);
    HRESULT hr = DFWD(EnumAdapterModes, a, f, i, m);
    if (SUCCEEDED(hr)) cz("mode|fmt=%s %ux%u@%u", fmt_s(f, fb), m->Width, m->Height, m->RefreshRate);
    return hr;
}
static HRESULT WINAPI c_GetAdapterDisplayMode(IDirect3D9Ex *self, UINT a, D3DDISPLAYMODE *m)
{
    char fb[16]; DHIT(GetAdapterDisplayMode);
    HRESULT hr = DFWD(GetAdapterDisplayMode, a, m);
    if (SUCCEEDED(hr)) cz("displaymode|fmt=%s %ux%u@%u", fmt_s(m->Format, fb), m->Width, m->Height, m->RefreshRate);
    return hr;
}

static HRESULT WINAPI c_d3d_GetDeviceCaps(IDirect3D9Ex *self, UINT a, D3DDEVTYPE t, D3DCAPS9 *caps)
{
    DHIT(GetDeviceCaps);
    HRESULT hr = DFWD(GetDeviceCaps, a, t, caps);
    static int once;   /* Source asks the IDirect3D9 (not the device): this is the caps struct M3 must reproduce */
    if (SUCCEEDED(hr) && !once++) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\caps-%s.bin", out_dir, tag); write_file(p, caps, sizeof *caps); }
    return hr;
}

/* ---------------------------------------------------------------- install */
int census_enabled(void) { return census_on; }

void census_install(void **d3d_vt, void **dev_vt, const char *dir, const char *tg)
{
    char mode[32] = "";
    GetEnvironmentVariableA("TF2MT_TRACE_MODE", mode, sizeof mode);
    if (strcmp(mode, "census")) return;
    census_on = 1;
    InitializeCriticalSection(&cz_lock);
    tls_slot = TlsAlloc();
    map = VirtualAlloc(NULL, sizeof(entry_t) * MAP_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    lstrcpynA(out_dir, dir, sizeof out_dir); lstrcpynA(tag, tg, sizeof tag);
    if (!GetEnvironmentVariableA("TF2MT_CORPUS_DIR", corpus_dir, sizeof corpus_dir))
        snprintf(corpus_dir, sizeof corpus_dir, "%s\\corpus", out_dir);
    CreateDirectoryA(corpus_dir, NULL);

    /* census thunks everywhere (per-thread method counts), then argument-capturing hooks on top */
    for (int i = 0; i < DEV_NMETHODS; i++) if (dev_vt[i] == dev_thunks[i]) dev_vt[i] = dev_cthunks[i];
    for (int i = 0; i < D3D_NMETHODS; i++) if (d3d_vt[i] == d3d_thunks[i]) d3d_vt[i] = d3d_cthunks[i];
#define HOOK(m) dev_vt[DEV_##m] = (void *)c_##m
    HOOK(CreateTexture); HOOK(CreateVolumeTexture); HOOK(CreateCubeTexture); HOOK(CreateVertexBuffer);
    HOOK(CreateIndexBuffer); HOOK(CreateRenderTarget); HOOK(CreateDepthStencilSurface); HOOK(CreateOffscreenPlainSurface);
    HOOK(CreateQuery); HOOK(CreateVertexShader); HOOK(CreatePixelShader); HOOK(CreateVertexDeclaration);
    HOOK(SetRenderState); HOOK(SetSamplerState); HOOK(SetTextureStageState); HOOK(SetTexture);
    HOOK(DrawPrimitive); HOOK(DrawIndexedPrimitive); HOOK(DrawPrimitiveUP); HOOK(DrawIndexedPrimitiveUP);
    HOOK(SetStreamSource); HOOK(SetStreamSourceFreq); HOOK(Clear); HOOK(StretchRect); HOOK(GetRenderTargetData);
    HOOK(UpdateSurface); HOOK(UpdateTexture); HOOK(ColorFill); HOOK(SetRenderTarget); HOOK(SetDepthStencilSurface);
    HOOK(SetViewport); HOOK(SetScissorRect); HOOK(SetClipPlane); HOOK(SetFVF);
    HOOK(SetVertexShaderConstantF); HOOK(SetVertexShaderConstantI); HOOK(SetVertexShaderConstantB);
    HOOK(SetPixelShaderConstantF); HOOK(SetPixelShaderConstantI); HOOK(SetPixelShaderConstantB);
    HOOK(CreateStateBlock); HOOK(GetDeviceCaps);
#define DHOOK(m) d3d_vt[D3D_##m] = (void *)c_##m
    d3d_vt[D3D_GetDeviceCaps] = (void *)c_d3d_GetDeviceCaps;
    DHOOK(CheckDeviceType); DHOOK(EnumAdapterModes); DHOOK(GetAdapterDisplayMode);
    d3d_vt[D3D_GetAdapterModeCount] = (void *)c_GetAdapterModeCount_;
    DHOOK(CheckDeviceFormat); DHOOK(CheckDeviceMultiSampleType); DHOOK(CheckDepthStencilMatch); DHOOK(CheckDeviceFormatConversion);
}

/* per-frame stats + implicit swapchain/backbuffer surfaces (game reads them with GetBackBuffer/GetRenderTarget) */
void census_frame(uint64_t draws)
{
    if (!census_on) return;
    cz("frame.draws|<=%u", pow2_bucket((unsigned)draws ? (unsigned)draws : 1));
    static unsigned n;
    if (++n % 3000 == 0) census_dump();
}
