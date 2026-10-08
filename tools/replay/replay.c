/* tools/replay — replays a .t9 capture (tools/trace capture mode, format tools/trace/t9.h) against a D3D9 provider.
 *   replay.exe <provider> <capture.t9> [--frames N] [--report out.txt] [--verbose]
 * <provider> is a DLL path (DXVK's d3d9.dll = oracle) or a module name (tf2mt.dll = under test). The replay window
 * has the captured back-buffer size; it runs as fast as the provider allows (no pacing).
 * Report: per-op call / failure counts, objects created per kind, bytes uploaded through locks, wall time.
 * Exit status: 0 = every call succeeded, 1 = some calls failed, 2 = could not run.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../trace/t9.h"

static const char *const op_names[T9_OP_COUNT] = {
    "?", "CREATE_DEVICE", "RESET", "PRESENT", "BEGIN_SCENE", "END_SCENE", "CLEAR", "SET_RENDER_STATE",
    "SET_SAMPLER_STATE", "SET_TSS", "SET_TEXTURE", "SET_STREAM_SOURCE", "SET_STREAM_FREQ", "SET_INDICES", "SET_VDECL",
    "SET_FVF", "SET_VS", "SET_PS", "VS_CONST_F", "VS_CONST_I", "VS_CONST_B", "PS_CONST_F", "PS_CONST_I", "PS_CONST_B",
    "SET_RT", "SET_DS", "SET_VIEWPORT", "SET_SCISSOR", "SET_CLIP_PLANE", "SET_TRANSFORM", "SET_MATERIAL",
    "SET_GAMMA_RAMP", "DRAW", "DRAW_INDEXED", "DRAW_UP", "DRAW_INDEXED_UP", "STRETCH_RECT", "UPDATE_SURFACE",
    "UPDATE_TEXTURE", "GET_RT_DATA", "COLOR_FILL", "CREATE_TEXTURE", "CREATE_CUBE", "CREATE_VOLUME", "CREATE_VB",
    "CREATE_IB", "CREATE_RT", "CREATE_DS", "CREATE_OFFSCREEN", "CREATE_QUERY", "CREATE_VS", "CREATE_PS",
    "CREATE_VDECL", "GET_SURFACE_LEVEL", "GET_CUBE_SURFACE", "GET_RT", "GET_DS", "GET_BACKBUFFER", "ADDREF",
    "RELEASE", "BUFFER_WRITE", "SURFACE_WRITE", "TEXTURE_WRITE", "VOLUME_WRITE", "QUERY_ISSUE", "QUERY_GETDATA",
    "GEN_MIPS", "EVICT_MANAGED", "LOCK_READ", "NOTE"};
_Static_assert(T9_NOTE == T9_OP_COUNT - 1, "op_names must list every t9_op");

enum { K_NONE, K_TEX, K_CUBE, K_VOL, K_VB, K_IB, K_SURF, K_QUERY, K_VS, K_PS, K_DECL };
typedef struct { IUnknown *p; uint8_t kind; } obj_t;
static obj_t *objs;
static uint32_t nobjs;
static uint8_t *ever;          /* id was ever bound (distinguishes "released early" from "never created") */

static uint64_t calls[T9_OP_COUNT], fails[T9_OP_COUNT];
static uint64_t upload_bytes, created[16], missing_obj, release_mismatch;
static FILE *rep;
static uint64_t dump_frames[64];
static unsigned ndump;
static char dump_dir[MAX_PATH];

/* Back buffer -> <dump_dir>\frame-<n>.ppm (RGB) through GetRenderTargetData: identical path on every provider. */
static void dump_frame(IDirect3DDevice9 *dev, uint64_t frame)
{
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    HRESULT hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL);
    IDirect3DSurface9 *src = bb, *rs = NULL;
    if (SUCCEEDED(hr) && d.MultiSampleType != D3DMULTISAMPLE_NONE) {   /* resolve first: GetRenderTargetData needs 1 sample */
        hr = IDirect3DDevice9_CreateRenderTarget(dev, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rs, NULL);
        if (SUCCEEDED(hr)) hr = IDirect3DDevice9_StretchRect(dev, bb, NULL, rs, NULL, D3DTEXF_NONE);
        src = rs;
    }
    if (SUCCEEDED(hr)) hr = IDirect3DDevice9_GetRenderTargetData(dev, src, sys);
    if (rs) IDirect3DSurface9_Release(rs);
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(hr) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\frame-%05llu.ppm", dump_dir, (unsigned long long)frame);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", d.Width, d.Height);
            uint8_t *row = malloc(d.Width * 3);
            for (UINT y = 0; y < d.Height; y++) {
                const uint8_t *s = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
                for (UINT x = 0; x < d.Width; x++) { row[x * 3] = s[x * 4 + 2]; row[x * 3 + 1] = s[x * 4 + 1]; row[x * 3 + 2] = s[x * 4]; }
                fwrite(row, 1, d.Width * 3, f);
            }
            free(row);
            fclose(f);
        }
        IDirect3DSurface9_UnlockRect(sys);
    } else printf("dump of frame %llu failed: 0x%08lx\n", (unsigned long long)frame, (unsigned long)hr);
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}
static int verbose;
static unsigned nerr_printed;

static void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    if (rep) { va_start(ap, fmt); vfprintf(rep, fmt, ap); va_end(ap); }
}

static void set_obj(uint32_t id, void *p, int kind)
{
    if (id >= nobjs) {
        uint32_t n = nobjs ? nobjs : 1 << 16;
        while (n <= id) n *= 2;
        objs = realloc(objs, n * sizeof *objs);
        memset(objs + nobjs, 0, (n - nobjs) * sizeof *objs);
        ever = realloc(ever, n);
        memset(ever + nobjs, 0, n - nobjs);
        nobjs = n;
    }
    /* lifetimes follow the capture: the replay holds one reference of its own on every mapped object and drops it
     * only when the captured refcount reaches 0 (providers count internal references differently) */
    if (objs[id].p != p) {
        if (p) IUnknown_AddRef((IUnknown *)p);
        if (objs[id].p) IUnknown_Release(objs[id].p);
    }
    objs[id].p = p;
    objs[id].kind = (uint8_t)kind;
    if (p) ever[id] = 1;
}
static uint32_t cur_op;
static uint64_t missing_by_op[T9_OP_COUNT], cur_rec;
static uint64_t missing_never, missing_released;
static void *obj(uint32_t id)
{
    if (!id) return NULL;
    if (id >= nobjs || !objs[id].p) {
        missing_obj++; missing_by_op[cur_op]++;
        if (id < nobjs && ever[id]) missing_released++; else missing_never++;
        if (verbose && missing_obj < 30) printf("missing id %u in record %llu %s\n", id, (unsigned long long)cur_rec, op_names[cur_op]);
        return NULL;
    }
    return objs[id].p;
}
static int kind_of(uint32_t id) { return id < nobjs ? objs[id].kind : 0; }

static void failed(uint32_t op, uint64_t rec, HRESULT hr, const char *what)
{
    fails[op]++;
    if (verbose || nerr_printed < 40) {
        nerr_printed++;
        report("FAIL record %llu %s: hr 0x%08lx %s\n", (unsigned long long)rec, op_names[op], (unsigned long)hr, what ? what : "");
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
static HWND make_window(uint32_t w, uint32_t h)
{
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "tf2mt_replay";
    RegisterClassA(&wc);
    RECT r = {0, 0, (LONG)w, (LONG)h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowA("tf2mt_replay", "tf2mt replay", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, r.right - r.left,
                            r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    return hw;
}
static void pump(void)
{
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

static D3DPRESENT_PARAMETERS from_pp(const t9_pp *t, HWND hw)
{
    D3DPRESENT_PARAMETERS p = {0};
    p.BackBufferWidth = t->BackBufferWidth; p.BackBufferHeight = t->BackBufferHeight;
    p.BackBufferFormat = (D3DFORMAT)t->BackBufferFormat; p.BackBufferCount = t->BackBufferCount;
    p.MultiSampleType = (D3DMULTISAMPLE_TYPE)t->MultiSampleType; p.MultiSampleQuality = t->MultiSampleQuality;
    p.SwapEffect = (D3DSWAPEFFECT)t->SwapEffect; p.hDeviceWindow = hw; p.Windowed = TRUE;
    p.EnableAutoDepthStencil = t->EnableAutoDepthStencil; p.AutoDepthStencilFormat = (D3DFORMAT)t->AutoDepthStencilFormat;
    p.Flags = t->Flags; p.FullScreen_RefreshRateInHz = 0;
    p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;   /* replay runs unpaced */
    return p;
}

static const RECT *opt_rect(const uint32_t *p, RECT *r)
{
    if (!p[0]) return NULL;
    memcpy(r, p + 1, 16);
    return r;
}

/* copy packed rows into a locked rect */
static void copy_rows(uint8_t *dst, INT pitch, const uint8_t *src, uint32_t row_bytes, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; y++) memcpy(dst + (size_t)y * (size_t)pitch, src + (size_t)y * row_bytes, row_bytes);
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: replay <provider dll> <capture.t9> [--frames N] [--report file] [--verbose]\n"); return 2; }
    uint64_t max_frames = UINT64_MAX;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--report") && i + 1 < argc) rep = fopen(argv[++i], "w");
        else if (!strcmp(argv[i], "--verbose")) verbose = 1;
        else if (!strcmp(argv[i], "--dump-dir") && i + 1 < argc) lstrcpynA(dump_dir, argv[++i], sizeof dump_dir);
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {   /* comma-separated frame numbers (0 = first Present) */
            for (char *t = strtok(argv[++i], ","); t && ndump < 64; t = strtok(NULL, ",")) dump_frames[ndump++] = strtoull(t, NULL, 10);
        }
    }
    HANDLE f = CreateFileA(argv[2], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { fprintf(stderr, "cannot open %s\n", argv[2]); return 2; }
    LARGE_INTEGER fsz;
    GetFileSizeEx(f, &fsz);
    HANDLE map = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    const uint8_t *data = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!data || fsz.QuadPart < (LONGLONG)sizeof(t9_header) || ((const t9_header *)data)->magic != T9_MAGIC) {
        fprintf(stderr, "%s: not a .t9 capture\n", argv[2]); return 2;
    }
    HMODULE prov = LoadLibraryA(argv[1]);
    if (!prov) { fprintf(stderr, "cannot load provider %s (err %lu)\n", argv[1], GetLastError()); return 2; }
    HRESULT (WINAPI *create_ex)(UINT, IDirect3D9Ex **) = (void *)GetProcAddress(prov, "Direct3DCreate9Ex");
    IDirect3D9 *(WINAPI *create)(UINT) = (void *)GetProcAddress(prov, "Direct3DCreate9");
    IDirect3D9Ex *d3d = NULL;
    if (create_ex) create_ex(D3D_SDK_VERSION, &d3d);
    if (!d3d && create) d3d = (IDirect3D9Ex *)create(D3D_SDK_VERSION);
    if (!d3d) { fprintf(stderr, "provider %s: Direct3DCreate9(Ex) failed\n", argv[1]); return 2; }

    IDirect3DDevice9 *dev = NULL;
    HWND hw = NULL;
    LARGE_INTEGER qpf, t0, t1;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    uint64_t frames = 0, nrec = 0, unknown = 0;
    size_t pos = sizeof(t9_header), end = (size_t)fsz.QuadPart;
    static uint8_t scratch[1 << 16];

    while (pos + 8 <= end && frames < max_frames) {
        uint32_t op = ((const uint32_t *)(data + pos))[0], sz = ((const uint32_t *)(data + pos))[1];
        if (pos + 8 + sz > end) { report("truncated record at offset %zu (capture still being written?)\n", pos); break; }
        const uint8_t *pl = data + pos + 8;
        const uint32_t *u = (const uint32_t *)pl;
        pos += 8 + sz;
        nrec++;
        if (op >= T9_OP_COUNT || !op) { unknown++; continue; }
        calls[op]++;
        cur_op = op; cur_rec = nrec;
        if (!dev && op != T9_CREATE_DEVICE && op != T9_NOTE) continue;   /* calls before device creation */
        HRESULT hr = D3D_OK;
        RECT r1, r2;
        switch (op) {
        case T9_CREATE_DEVICE: {
            const t9_create_device *c = (const void *)pl;
            if (dev) { report("second CREATE_DEVICE ignored\n"); break; }
            hw = make_window(c->pp.BackBufferWidth, c->pp.BackBufferHeight);
            D3DPRESENT_PARAMETERS pp = from_pp(&c->pp, hw);
            hr = IDirect3D9Ex_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hw, c->behavior, &pp, &dev);
            if (FAILED(hr)) { report("CreateDevice failed: 0x%08lx\n", (unsigned long)hr); return 2; }
            report("device: %ux%u fmt %u ms %u behavior 0x%x (captured %s)\n", c->pp.BackBufferWidth, c->pp.BackBufferHeight,
                   c->pp.BackBufferFormat, c->pp.MultiSampleType, c->behavior, c->ex ? "CreateDeviceEx" : "CreateDevice");
            break;
        }
        case T9_RESET: { D3DPRESENT_PARAMETERS pp = from_pp((const t9_pp *)pl, hw); hr = IDirect3DDevice9_Reset(dev, &pp); break; }
        case T9_PRESENT:
            for (unsigned k = 0; k < ndump && dump_dir[0]; k++) if (dump_frames[k] == frames) dump_frame(dev, frames);
            hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
            frames++;
            pump();
            break;
        case T9_BEGIN_SCENE: hr = IDirect3DDevice9_BeginScene(dev); break;
        case T9_END_SCENE: hr = IDirect3DDevice9_EndScene(dev); break;
        case T9_CLEAR: {
            const t9_clear *c = (const void *)pl;
            hr = IDirect3DDevice9_Clear(dev, c->count, c->count ? (const D3DRECT *)(c + 1) : NULL, c->flags, c->color, c->z, c->stencil);
            break;
        }
        case T9_SET_RENDER_STATE: hr = IDirect3DDevice9_SetRenderState(dev, (D3DRENDERSTATETYPE)u[0], u[1]); break;
        case T9_SET_SAMPLER_STATE: hr = IDirect3DDevice9_SetSamplerState(dev, u[0], (D3DSAMPLERSTATETYPE)u[1], u[2]); break;
        case T9_SET_TSS: hr = IDirect3DDevice9_SetTextureStageState(dev, u[0], (D3DTEXTURESTAGESTATETYPE)u[1], u[2]); break;
        case T9_SET_TEXTURE: hr = IDirect3DDevice9_SetTexture(dev, u[0], obj(u[1])); break;
        case T9_SET_STREAM_SOURCE: hr = IDirect3DDevice9_SetStreamSource(dev, u[0], obj(u[1]), u[2], u[3]); break;
        case T9_SET_STREAM_FREQ: hr = IDirect3DDevice9_SetStreamSourceFreq(dev, u[0], u[1]); break;
        case T9_SET_INDICES: hr = IDirect3DDevice9_SetIndices(dev, obj(u[0])); break;
        case T9_SET_VDECL: hr = IDirect3DDevice9_SetVertexDeclaration(dev, obj(u[0])); break;
        case T9_SET_FVF: hr = IDirect3DDevice9_SetFVF(dev, u[0]); break;
        case T9_SET_VS: hr = IDirect3DDevice9_SetVertexShader(dev, obj(u[0])); break;
        case T9_SET_PS: hr = IDirect3DDevice9_SetPixelShader(dev, obj(u[0])); break;
        case T9_VS_CONST_F: hr = IDirect3DDevice9_SetVertexShaderConstantF(dev, u[0], (const float *)(u + 2), u[1]); break;
        case T9_VS_CONST_I: hr = IDirect3DDevice9_SetVertexShaderConstantI(dev, u[0], (const int *)(u + 2), u[1]); break;
        case T9_VS_CONST_B: hr = IDirect3DDevice9_SetVertexShaderConstantB(dev, u[0], (const BOOL *)(u + 2), u[1]); break;
        case T9_PS_CONST_F: hr = IDirect3DDevice9_SetPixelShaderConstantF(dev, u[0], (const float *)(u + 2), u[1]); break;
        case T9_PS_CONST_I: hr = IDirect3DDevice9_SetPixelShaderConstantI(dev, u[0], (const int *)(u + 2), u[1]); break;
        case T9_PS_CONST_B: hr = IDirect3DDevice9_SetPixelShaderConstantB(dev, u[0], (const BOOL *)(u + 2), u[1]); break;
        case T9_SET_RT: hr = IDirect3DDevice9_SetRenderTarget(dev, u[0], obj(u[1])); break;
        case T9_SET_DS: hr = IDirect3DDevice9_SetDepthStencilSurface(dev, obj(u[0])); break;
        case T9_SET_VIEWPORT: hr = IDirect3DDevice9_SetViewport(dev, (const D3DVIEWPORT9 *)pl); break;
        case T9_SET_SCISSOR: hr = IDirect3DDevice9_SetScissorRect(dev, (const RECT *)pl); break;
        case T9_SET_CLIP_PLANE: hr = IDirect3DDevice9_SetClipPlane(dev, u[0], (const float *)(u + 1)); break;
        case T9_SET_TRANSFORM: hr = IDirect3DDevice9_SetTransform(dev, (D3DTRANSFORMSTATETYPE)u[0], (const D3DMATRIX *)(u + 1)); break;
        case T9_SET_MATERIAL: hr = IDirect3DDevice9_SetMaterial(dev, (const D3DMATERIAL9 *)pl); break;
        case T9_SET_GAMMA_RAMP: IDirect3DDevice9_SetGammaRamp(dev, u[0], u[1], (const D3DGAMMARAMP *)(u + 2)); break;
        case T9_DRAW: hr = IDirect3DDevice9_DrawPrimitive(dev, (D3DPRIMITIVETYPE)u[0], u[1], u[2]); break;
        case T9_DRAW_INDEXED:
            hr = IDirect3DDevice9_DrawIndexedPrimitive(dev, (D3DPRIMITIVETYPE)u[0], (INT)u[1], u[2], u[3], u[4], u[5]);
            break;
        case T9_DRAW_UP: hr = IDirect3DDevice9_DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)u[0], u[1], u + 4, u[2]); break;
        case T9_DRAW_INDEXED_UP:
            hr = IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, (D3DPRIMITIVETYPE)u[0], u[1], u[2], u[3], u + 8, (D3DFORMAT)u[4],
                                                         (const uint8_t *)(u + 8) + u[6], u[5]);
            break;
        case T9_STRETCH_RECT:
            hr = IDirect3DDevice9_StretchRect(dev, obj(u[0]), opt_rect(u + 1, &r1), obj(u[6]), opt_rect(u + 7, &r2), (D3DTEXTUREFILTERTYPE)u[12]);
            break;
        case T9_UPDATE_SURFACE: {
            POINT pt = {(LONG)u[8], (LONG)u[9]};
            hr = IDirect3DDevice9_UpdateSurface(dev, obj(u[0]), opt_rect(u + 1, &r1), obj(u[6]), u[7] ? &pt : NULL);
            break;
        }
        case T9_UPDATE_TEXTURE: hr = IDirect3DDevice9_UpdateTexture(dev, obj(u[0]), obj(u[1])); break;
        case T9_GET_RT_DATA: hr = IDirect3DDevice9_GetRenderTargetData(dev, obj(u[0]), obj(u[1])); break;
        case T9_COLOR_FILL: hr = IDirect3DDevice9_ColorFill(dev, obj(u[0]), opt_rect(u + 1, &r1), u[6]); break;

        case T9_CREATE_TEXTURE: {
            IDirect3DTexture9 *t = NULL;
            hr = IDirect3DDevice9_CreateTexture(dev, u[1], u[2], u[3], u[4], (D3DFORMAT)u[5], (D3DPOOL)u[6], &t, NULL);
            set_obj(u[0], t, K_TEX); created[K_TEX] += SUCCEEDED(hr);
            if (FAILED(hr)) { char b[96]; snprintf(b, sizeof b, "%ux%u lv %u usage 0x%x fmt %u pool %u", u[1], u[2], u[3], u[4], u[5], u[6]); failed(op, nrec, hr, b); hr = D3D_OK; }
            break;
        }
        case T9_CREATE_CUBE: {
            IDirect3DCubeTexture9 *t = NULL;
            hr = IDirect3DDevice9_CreateCubeTexture(dev, u[1], u[2], u[3], (D3DFORMAT)u[4], (D3DPOOL)u[5], &t, NULL);
            set_obj(u[0], t, K_CUBE); created[K_CUBE] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_VOLUME: {
            IDirect3DVolumeTexture9 *t = NULL;
            hr = IDirect3DDevice9_CreateVolumeTexture(dev, u[1], u[2], u[3], u[4], u[5], (D3DFORMAT)u[6], (D3DPOOL)u[7], &t, NULL);
            set_obj(u[0], t, K_VOL); created[K_VOL] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_VB: {
            IDirect3DVertexBuffer9 *b = NULL;
            hr = IDirect3DDevice9_CreateVertexBuffer(dev, u[1], u[2], u[3], (D3DPOOL)u[4], &b, NULL);
            set_obj(u[0], b, K_VB); created[K_VB] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_IB: {
            IDirect3DIndexBuffer9 *b = NULL;
            hr = IDirect3DDevice9_CreateIndexBuffer(dev, u[1], u[2], (D3DFORMAT)u[3], (D3DPOOL)u[4], &b, NULL);
            set_obj(u[0], b, K_IB); created[K_IB] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_RT: {
            IDirect3DSurface9 *s = NULL;
            hr = IDirect3DDevice9_CreateRenderTarget(dev, u[1], u[2], (D3DFORMAT)u[3], (D3DMULTISAMPLE_TYPE)u[4], u[5], u[6], &s, NULL);
            set_obj(u[0], s, K_SURF); created[K_SURF] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_DS: {
            IDirect3DSurface9 *s = NULL;
            hr = IDirect3DDevice9_CreateDepthStencilSurface(dev, u[1], u[2], (D3DFORMAT)u[3], (D3DMULTISAMPLE_TYPE)u[4], u[5], u[6], &s, NULL);
            set_obj(u[0], s, K_SURF); created[K_SURF] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_OFFSCREEN: {
            IDirect3DSurface9 *s = NULL;
            hr = IDirect3DDevice9_CreateOffscreenPlainSurface(dev, u[1], u[2], (D3DFORMAT)u[3], (D3DPOOL)u[4], &s, NULL);
            set_obj(u[0], s, K_SURF); created[K_SURF] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_QUERY: {
            IDirect3DQuery9 *q = NULL;
            hr = IDirect3DDevice9_CreateQuery(dev, (D3DQUERYTYPE)u[1], &q);
            set_obj(u[0], q, K_QUERY); created[K_QUERY] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_VS: {
            IDirect3DVertexShader9 *s = NULL;
            hr = IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)(u + 2), &s);
            set_obj(u[0], s, K_VS); created[K_VS] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_PS: {
            IDirect3DPixelShader9 *s = NULL;
            hr = IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)(u + 2), &s);
            set_obj(u[0], s, K_PS); created[K_PS] += SUCCEEDED(hr);
            break;
        }
        case T9_CREATE_VDECL: {
            IDirect3DVertexDeclaration9 *d = NULL;
            hr = IDirect3DDevice9_CreateVertexDeclaration(dev, (const D3DVERTEXELEMENT9 *)(u + 2), &d);
            set_obj(u[0], d, K_DECL); created[K_DECL] += SUCCEEDED(hr);
            break;
        }
        case T9_GET_SURFACE_LEVEL: {
            IDirect3DSurface9 *s = NULL;
            IDirect3DTexture9 *t = obj(u[1]);
            hr = t ? IDirect3DTexture9_GetSurfaceLevel(t, u[2], &s) : D3DERR_INVALIDCALL;
            set_obj(u[0], s, K_SURF);
            break;
        }
        case T9_GET_CUBE_SURFACE: {
            IDirect3DSurface9 *s = NULL;
            IDirect3DCubeTexture9 *t = obj(u[1]);
            hr = t ? IDirect3DCubeTexture9_GetCubeMapSurface(t, (D3DCUBEMAP_FACES)u[2], u[3], &s) : D3DERR_INVALIDCALL;
            set_obj(u[0], s, K_SURF);
            break;
        }
        case T9_GET_RT: { IDirect3DSurface9 *s = NULL; hr = IDirect3DDevice9_GetRenderTarget(dev, u[1], &s); set_obj(u[0], s, K_SURF); break; }
        case T9_GET_DS: { IDirect3DSurface9 *s = NULL; hr = IDirect3DDevice9_GetDepthStencilSurface(dev, &s); set_obj(u[0], s, K_SURF); break; }
        case T9_GET_BACKBUFFER: {
            IDirect3DSurface9 *s = NULL;
            hr = IDirect3DDevice9_GetBackBuffer(dev, u[1], u[2], (D3DBACKBUFFER_TYPE)u[3], &s);
            set_obj(u[0], s, K_SURF);
            break;
        }
        case T9_ADDREF: { IUnknown *o = obj(u[0]); if (o) IUnknown_AddRef(o); break; }
        case T9_RELEASE: {
            IUnknown *o = obj(u[0]);
            if (!o) break;
            ULONG c = IUnknown_Release(o) - 1;   /* minus the replay's own reference */
            if (c != u[1] && release_mismatch++ < 20 && verbose)
                printf("release count differs: record %llu id %u replay %lu capture %u\n", (unsigned long long)nrec, u[0], (unsigned long)c, u[1]);
            if (u[1] == 0) { IUnknown_Release(o); objs[u[0]].p = NULL; }
            break;
        }
        case T9_BUFFER_WRITE: {
            void *p = NULL;
            void *b = obj(u[0]);
            if (!b) { hr = D3DERR_INVALIDCALL; break; }
            if (kind_of(u[0]) == K_VB) {
                hr = IDirect3DVertexBuffer9_Lock((IDirect3DVertexBuffer9 *)b, u[1], u[2], &p, u[3]);
                if (SUCCEEDED(hr)) { memcpy(p, u + 4, u[2]); hr = IDirect3DVertexBuffer9_Unlock((IDirect3DVertexBuffer9 *)b); }
            } else {
                hr = IDirect3DIndexBuffer9_Lock((IDirect3DIndexBuffer9 *)b, u[1], u[2], &p, u[3]);
                if (SUCCEEDED(hr)) { memcpy(p, u + 4, u[2]); hr = IDirect3DIndexBuffer9_Unlock((IDirect3DIndexBuffer9 *)b); }
            }
            upload_bytes += u[2];
            break;
        }
        case T9_SURFACE_WRITE: case T9_TEXTURE_WRITE: {
            const uint32_t *pre = u;
            const t9_rect_write *w = (const void *)(op == T9_TEXTURE_WRITE ? u + 3 : u);
            const uint8_t *rows = (const uint8_t *)(w + 1);
            void *o = obj(op == T9_TEXTURE_WRITE ? pre[0] : w->id);
            if (!o) { hr = D3DERR_INVALIDCALL; break; }
            RECT rc = {w->left, w->top, w->right, w->bottom};
            D3DLOCKED_RECT lr;
            if (op == T9_SURFACE_WRITE) {
                hr = IDirect3DSurface9_LockRect((IDirect3DSurface9 *)o, &lr, w->has_rect ? &rc : NULL, w->flags);
                if (SUCCEEDED(hr)) { copy_rows(lr.pBits, lr.Pitch, rows, w->row_bytes, w->rows); hr = IDirect3DSurface9_UnlockRect((IDirect3DSurface9 *)o); }
            } else if (kind_of(pre[0]) == K_CUBE) {
                hr = IDirect3DCubeTexture9_LockRect((IDirect3DCubeTexture9 *)o, (D3DCUBEMAP_FACES)pre[1], pre[2], &lr, w->has_rect ? &rc : NULL, w->flags);
                if (SUCCEEDED(hr)) { copy_rows(lr.pBits, lr.Pitch, rows, w->row_bytes, w->rows); hr = IDirect3DCubeTexture9_UnlockRect((IDirect3DCubeTexture9 *)o, (D3DCUBEMAP_FACES)pre[1], pre[2]); }
            } else {
                hr = IDirect3DTexture9_LockRect((IDirect3DTexture9 *)o, pre[2], &lr, w->has_rect ? &rc : NULL, w->flags);
                if (SUCCEEDED(hr)) { copy_rows(lr.pBits, lr.Pitch, rows, w->row_bytes, w->rows); hr = IDirect3DTexture9_UnlockRect((IDirect3DTexture9 *)o, pre[2]); }
            }
            upload_bytes += (uint64_t)w->row_bytes * w->rows;
            break;
        }
        case T9_VOLUME_WRITE: {
            const t9_box_write *w = (const void *)pl;
            IDirect3DVolumeTexture9 *o = obj(w->id);
            if (!o) { hr = D3DERR_INVALIDCALL; break; }
            D3DBOX bx = {w->left, w->top, w->right, w->bottom, w->front, w->back};
            D3DLOCKED_BOX lb;
            hr = IDirect3DVolumeTexture9_LockBox(o, w->level, &lb, w->has_box ? &bx : NULL, w->flags);
            if (SUCCEEDED(hr)) {
                const uint8_t *src = (const uint8_t *)(w + 1);
                for (uint32_t z = 0; z < w->slices; z++)
                    copy_rows((uint8_t *)lb.pBits + (size_t)z * (size_t)lb.SlicePitch, lb.RowPitch,
                              src + (size_t)z * w->rows * w->row_bytes, w->row_bytes, w->rows);
                hr = IDirect3DVolumeTexture9_UnlockBox(o, w->level);
            }
            upload_bytes += (uint64_t)w->row_bytes * w->rows * w->slices;
            break;
        }
        case T9_LOCK_READ: {
            void *o = obj(u[0]);
            if (o && u[1] == 1 && kind_of(u[0]) == K_SURF) {
                D3DLOCKED_RECT lr;
                hr = IDirect3DSurface9_LockRect((IDirect3DSurface9 *)o, &lr, NULL, D3DLOCK_READONLY);
                if (SUCCEEDED(hr)) hr = IDirect3DSurface9_UnlockRect((IDirect3DSurface9 *)o);
            }
            break;
        }
        case T9_QUERY_ISSUE: { IDirect3DQuery9 *q = obj(u[0]); hr = q ? IDirect3DQuery9_Issue(q, u[1]) : D3DERR_INVALIDCALL; break; }
        case T9_QUERY_GETDATA: {
            IDirect3DQuery9 *q = obj(u[0]);
            if (!q) { hr = D3DERR_INVALIDCALL; break; }
            hr = IDirect3DQuery9_GetData(q, u[1] ? scratch : NULL, u[1] < sizeof scratch ? u[1] : 0, u[2]);
            if (hr == S_FALSE) hr = D3D_OK;   /* not ready yet: timing differs from the capture, not an error */
            break;
        }
        case T9_GEN_MIPS: { IDirect3DTexture9 *t = obj(u[0]); if (t) IDirect3DTexture9_GenerateMipSubLevels(t); break; }
        case T9_EVICT_MANAGED: hr = IDirect3DDevice9_EvictManagedResources(dev); break;
        case T9_NOTE: report("note: %.*s\n", (int)sz, (const char *)pl); break;
        default: unknown++; break;
        }
        if (FAILED(hr)) failed(op, nrec, hr, NULL);
    }
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)qpf.QuadPart;

    uint64_t total_fail = 0;
    report("\nreplayed %llu records, %llu frames in %.2f s (%.1f fps)\n", (unsigned long long)nrec, (unsigned long long)frames,
           secs, frames / (secs > 0 ? secs : 1));
    report("uploads through locks: %.1f MB (%.1f MB/s)\n", upload_bytes / 1e6, upload_bytes / 1e6 / (secs > 0 ? secs : 1));
    report("created: tex %llu cube %llu volume %llu vb %llu ib %llu surface %llu query %llu vs %llu ps %llu decl %llu\n",
           (unsigned long long)created[K_TEX], (unsigned long long)created[K_CUBE], (unsigned long long)created[K_VOL],
           (unsigned long long)created[K_VB], (unsigned long long)created[K_IB], (unsigned long long)created[K_SURF],
           (unsigned long long)created[K_QUERY], (unsigned long long)created[K_VS], (unsigned long long)created[K_PS],
           (unsigned long long)created[K_DECL]);
    report("missing-object references %llu, release-count differences %llu, unknown records %llu\n",
           (unsigned long long)missing_obj, (unsigned long long)release_mismatch, (unsigned long long)unknown);
    report("missing: %llu never created, %llu already released\n", (unsigned long long)missing_never, (unsigned long long)missing_released);
    for (int i = 1; i < T9_OP_COUNT; i++)
        if (missing_by_op[i]) report("  missing in %-18s %llu\n", op_names[i], (unsigned long long)missing_by_op[i]);
    report("%-20s %12s %10s\n", "op", "calls", "failed");
    for (int i = 1; i < T9_OP_COUNT; i++) {
        if (calls[i]) report("%-20s %12llu %10llu\n", op_names[i], (unsigned long long)calls[i], (unsigned long long)fails[i]);
        total_fail += fails[i];
    }
    report("RESULT %s (%llu failed calls)\n", total_fail ? "FAIL" : "OK", (unsigned long long)total_fail);
    if (rep) fclose(rep);
    if (dev) IDirect3DDevice9_Release(dev);
    IDirect3D9Ex_Release(d3d);
    return total_fail ? 1 : 0;
}
