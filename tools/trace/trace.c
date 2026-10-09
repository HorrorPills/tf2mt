/* tools/trace — d3d9.dll proxy (x86_64 PE), timing-only mode.
 *
 * Loads the oracle (d3d9_ref.dll next to this DLL = a copy of the runtime's DXVK d3d9.dll),
 * wraps IDirect3D9(Ex) and IDirect3DDevice9(Ex) with generic forwarding thunks (thunks.S,
 * generated from mingw-w64's d3d9.h) and records one CSV row per Present:
 *   frame, t_ms, ms_between_presents, ms_in_present, draws, calls, thread_cpu_ms, proc_cpu_ms, tid
 * Output: %TF2MT_TRACE_DIR%\frames-<TF2MT_TAG>.csv (+ .info.txt with device parameters).
 * Nothing here touches game memory; we only sit between shaderapidx9 and the D3D9 provider.
 */
#define COBJMACROS
#define INITGUID
#include <math.h>
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include "methods.h"
#include "ifaces.h"
#include "trace_common.h"

uint64_t d3d_calls[D3D_NMETHODS];
uint64_t dev_calls[DEV_NMETHODS];

static void *d3d_vtbl[D3D_NMETHODS], *dev_vtbl[DEV_NMETHODS];
static HMODULE ref_dll;

/* ---------------------------------------------------------------- logging */
static HANDLE out_csv = INVALID_HANDLE_VALUE, out_info = INVALID_HANDLE_VALUE;
static char csv_buf[1 << 20];
static size_t csv_len;
static LARGE_INTEGER qpf, t0, last_present;
static uint64_t last_draws_census;
static uint64_t frame_no, last_draws, last_calls, last_thread_cpu, last_proc_cpu;
static uint64_t prev_dev_calls[DEV_NMETHODS];   /* per-frame deltas for the slow-frame profile */
/* camera-change detection: Source passes the view-projection matrix in VS constants c8..c11. We hash it at
 * its first upload in each frame; a frame whose hash equals the previous frame's shows an unchanged camera. */
static volatile uint64_t cam_hash_frame, cam_hash_prev;
static volatile int cam_seen;
static volatile float cam_yaw = -999.f, cam_pitch = -999.f;   /* from the view-projection matrix's w row (camera forward) */
static CRITICAL_SECTION log_lock;

static void write_all(HANDLE h, const char *p, size_t n)
{
    DWORD w;
    while (n && WriteFile(h, p, (DWORD)n, &w, NULL) && w) { p += w; n -= w; }
}

void info(const char *fmt, ...)
{
    char b[2048];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (out_info != INVALID_HANDLE_VALUE && n > 0) write_all(out_info, b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}

static void flush_csv(void)
{
    if (out_csv != INVALID_HANDLE_VALUE && csv_len) write_all(out_csv, csv_buf, csv_len);
    csv_len = 0;
}

static HANDLE open_out(const char *dir, const char *tag, const char *suffix)
{
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\frames-%s%s", dir, tag, suffix);
    return CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static char dir[MAX_PATH] = "Z:\\tmp", tag[128] = "untagged";
static void log_open(void)
{
    GetEnvironmentVariableA("TF2MT_TRACE_DIR", dir, sizeof dir);
    if (!GetEnvironmentVariableA("TF2MT_TAG", tag, sizeof tag))
        snprintf(tag, sizeof tag, "%lu", GetCurrentProcessId());
    out_csv = open_out(dir, tag, ".csv");
    out_info = open_out(dir, tag, ".info.txt");
    static const char hdr[] = "frame,t_ms,ms_between_presents,ms_in_present,draws,calls,thread_cpu_ms,proc_cpu_ms,tid,cam_changed,cam_yaw,cam_pitch\n";
    if (out_csv != INVALID_HANDLE_VALUE) write_all(out_csv, hdr, sizeof hdr - 1);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    info("tf2mt trace (timing-only) pid %lu\n", GetCurrentProcessId());
}

static uint64_t ft_100ns(FILETIME a, FILETIME b)
{
    return (((uint64_t)a.dwHighDateTime << 32) | a.dwLowDateTime) + (((uint64_t)b.dwHighDateTime << 32) | b.dwLowDateTime);
}

static double qpc_ms(LONGLONG d) { return (double)d * 1000.0 / (double)qpf.QuadPart; }

static void dump_call_counts(void)
{
    info("--- device method call counts\n");
    for (int i = 0; i < DEV_NMETHODS; i++)
        if (dev_calls[i]) info("%-32s %llu\n", dev_method_names[i], (unsigned long long)dev_calls[i]);
    info("--- d3d9 method call counts\n");
    for (int i = 0; i < D3D_NMETHODS; i++)
        if (d3d_calls[i]) info("%-32s %llu\n", d3d_method_names[i], (unsigned long long)d3d_calls[i]);
}

/* one CSV row per present; called with present start/end timestamps */
static void record_frame(LARGE_INTEGER start, LARGE_INTEGER end)
{
    static DWORD named_tid, n_named;
    if (named_tid != GetCurrentThreadId()) {   /* lets threadmon (macOS side) identify D3D-calling threads */
        WCHAR name[32];
        named_tid = GetCurrentThreadId();
        swprintf(name, 32, L"d3d9-present-%lu", ++n_named);
        SetThreadDescription(GetCurrentThread(), name);
        info("present thread %lu = tid %lu at frame %llu\n", n_named, named_tid, (unsigned long long)frame_no);
    }
    FILETIME c, e, k, u;
    uint64_t tcpu = 0, pcpu = 0;
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) tcpu = ft_100ns(k, u);
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) pcpu = ft_100ns(k, u);
    uint64_t draws = dev_calls[DEV_DrawPrimitive] + dev_calls[DEV_DrawIndexedPrimitive]
                   + dev_calls[DEV_DrawPrimitiveUP] + dev_calls[DEV_DrawIndexedPrimitiveUP];
    uint64_t calls = 0;
    for (int i = 0; i < DEV_NMETHODS; i++) calls += dev_calls[i];

    int cam_changed = cam_seen ? (cam_hash_frame != cam_hash_prev) : -1;
    if (cam_seen) cam_hash_prev = cam_hash_frame;
    cam_seen = 0;
    EnterCriticalSection(&log_lock);
    double between = last_present.QuadPart ? qpc_ms(start.QuadPart - last_present.QuadPart) : 0.0;
    csv_len += (size_t)snprintf(csv_buf + csv_len, sizeof csv_buf - csv_len,
        "%llu,%.4f,%.4f,%.4f,%llu,%llu,%.3f,%.3f,%lu,%d,%.4f,%.4f\n",
        (unsigned long long)frame_no++, qpc_ms(start.QuadPart - t0.QuadPart), between,
        qpc_ms(end.QuadPart - start.QuadPart),
        (unsigned long long)(draws - last_draws), (unsigned long long)(calls - last_calls),
        last_thread_cpu ? (tcpu - last_thread_cpu) / 1e4 : 0.0, last_proc_cpu ? (pcpu - last_proc_cpu) / 1e4 : 0.0,
        GetCurrentThreadId(), cam_changed, (double)cam_yaw, (double)cam_pitch);
    if (between > 100.0) {   /* slow frame: log which device methods it called (and how often) */
        char line[4096];
        int n = snprintf(line, sizeof line, "SLOW frame %llu %.1f ms:", (unsigned long long)frame_no - 1, between);
        for (int i = 0; i < DEV_NMETHODS && n < (int)sizeof line - 64; i++)
            if (dev_calls[i] != prev_dev_calls[i])
                n += snprintf(line + n, sizeof line - n, " %s=%llu", dev_method_names[i],
                              (unsigned long long)(dev_calls[i] - prev_dev_calls[i]));
        info("%s\n", line);
    }
    memcpy(prev_dev_calls, dev_calls, sizeof prev_dev_calls);
    last_present = start; last_draws = draws; last_calls = calls;
    last_thread_cpu = tcpu; last_proc_cpu = pcpu;
    if (csv_len > (64u << 10)) flush_csv(); /* ~700 frames; one WriteFile */
    census_frame(draws - last_draws_census);
    last_draws_census = draws;
    LeaveCriticalSection(&log_lock);
}

/* ---------------------------------------------------------------- wrappers */
static wrap_t *wrap(void **vtbl, IUnknown *inner)
{
    wrap_t *w = HeapAlloc(GetProcessHeap(), 0, sizeof *w);
    w->vtbl = vtbl; w->inner = inner;
    return w;
}

/* device */
static HRESULT WINAPI dev_QueryInterface(IDirect3DDevice9Ex *self, REFIID riid, void **out)
{
    dev_calls[DEV_QueryInterface]++;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3DDevice9)
        || IsEqualGUID(riid, &IID_IDirect3DDevice9Ex)) {
        void *tmp;
        HRESULT hr = IDirect3DDevice9Ex_QueryInterface(INNER(IDirect3DDevice9Ex, self), riid, &tmp);
        if (FAILED(hr)) { *out = NULL; return hr; }
        *out = self; /* the reference taken on the inner object is ours */
        return hr;
    }
    return IDirect3DDevice9Ex_QueryInterface(INNER(IDirect3DDevice9Ex, self), riid, out);
}

static ULONG WINAPI dev_Release(IDirect3DDevice9Ex *self)
{
    dev_calls[DEV_Release]++;
    ULONG r = IDirect3DDevice9Ex_Release(INNER(IDirect3DDevice9Ex, self));
    if (!r) {
        EnterCriticalSection(&log_lock); flush_csv(); LeaveCriticalSection(&log_lock);
        info("device released after %llu frames\n", (unsigned long long)frame_no);
        census_dump();
        capture_close();
        dump_call_counts();
        HeapFree(GetProcessHeap(), 0, self);
    }
    return r;
}

static HRESULT WINAPI dev_Present(IDirect3DDevice9Ex *self, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty)
{
    LARGE_INTEGER a, b;
    dev_calls[DEV_Present]++;
    if (census_enabled()) census_hit((1u << 16) | DEV_Present);
    QueryPerformanceCounter(&a);
    HRESULT hr = IDirect3DDevice9Ex_Present(INNER(IDirect3DDevice9Ex, self), src, dst, wnd, dirty);
    QueryPerformanceCounter(&b);
    record_frame(a, b);
    return hr;
}

static HRESULT WINAPI dev_PresentEx(IDirect3DDevice9Ex *self, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty, DWORD flags)
{
    LARGE_INTEGER a, b;
    dev_calls[DEV_PresentEx]++;
    QueryPerformanceCounter(&a);
    HRESULT hr = IDirect3DDevice9Ex_PresentEx(INNER(IDirect3DDevice9Ex, self), src, dst, wnd, dirty, flags);
    QueryPerformanceCounter(&b);
    record_frame(a, b);
    return hr;
}

static HRESULT WINAPI dev_SetVertexShaderConstantF(IDirect3DDevice9Ex *self, UINT start, const float *c, UINT count)
{
    dev_calls[DEV_SetVertexShaderConstantF]++;
    if (!cam_seen && start <= 8 && start + count >= 12) {
        const uint32_t *w = (const uint32_t *)(c + (8 - start) * 4);
        uint64_t h = 1469598103934665603ull;            /* FNV-1a over the 16 floats */
        for (int i = 0; i < 16; i++) { h ^= w[i]; h *= 1099511628211ull; }
        cam_hash_frame = h; cam_seen = 1;
        const float *wr = c + (11 - start) * 4;           /* c11 = clip-space w row = camera forward vector */
        float fx = wr[0], fy = wr[1], fz = wr[2];
        if (fx != 0.f || fy != 0.f) {
            cam_yaw = atan2f(fy, fx) * 57.29578f;
            cam_pitch = atan2f(fz, sqrtf(fx * fx + fy * fy)) * 57.29578f;
        }
    }
    return IDirect3DDevice9Ex_SetVertexShaderConstantF(INNER(IDirect3DDevice9Ex, self), start, c, count);
}

static void log_pp(const char *what, const D3DPRESENT_PARAMETERS *pp)
{
    if (!pp) return;
    info("%s: %ux%u fmt %u count %u ms %u/%lu swap %u hwnd %p windowed %d autoDS %d dsfmt %u flags 0x%lx refresh %u interval 0x%x\n",
         what, pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount,
         pp->MultiSampleType, pp->MultiSampleQuality, pp->SwapEffect, (void *)pp->hDeviceWindow, pp->Windowed,
         pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz,
         pp->PresentationInterval);
}

static HRESULT WINAPI dev_Reset(IDirect3DDevice9Ex *self, D3DPRESENT_PARAMETERS *pp)
{
    dev_calls[DEV_Reset]++;
    log_pp("Reset", pp);
    return IDirect3DDevice9Ex_Reset(INNER(IDirect3DDevice9Ex, self), pp);
}

static HRESULT WINAPI dev_ResetEx(IDirect3DDevice9Ex *self, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode)
{
    dev_calls[DEV_ResetEx]++;
    log_pp("ResetEx", pp);
    return IDirect3DDevice9Ex_ResetEx(INNER(IDirect3DDevice9Ex, self), pp, mode);
}

/* IDirect3D9(Ex) */
static ULONG WINAPI d3d_Release(IDirect3D9Ex *self)
{
    d3d_calls[D3D_Release]++;
    ULONG r = IDirect3D9Ex_Release(INNER(IDirect3D9Ex, self));
    if (!r) HeapFree(GetProcessHeap(), 0, self);
    return r;
}

static HRESULT WINAPI d3d_GetAdapterIdentifier(IDirect3D9Ex *self, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id)
{
    d3d_calls[D3D_GetAdapterIdentifier]++;
    HRESULT hr = IDirect3D9Ex_GetAdapterIdentifier(INNER(IDirect3D9Ex, self), adapter, flags, id);
    static int once;
    if (SUCCEEDED(hr) && !once) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\adapter-%s.bin", dir, tag);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { write_all(h, (const char *)id, sizeof *id); CloseHandle(h); }
    }
    if (SUCCEEDED(hr) && !once++)
        info("adapter %u: '%s' driver '%s' vendor 0x%04lx device 0x%04lx subsys 0x%08lx rev %lu\n", adapter,
             id->Description, id->Driver, id->VendorId, id->DeviceId, id->SubSysId, id->Revision);
    return hr;
}

static HRESULT WINAPI d3d_CreateDevice(IDirect3D9Ex *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **out)
{
    d3d_calls[D3D_CreateDevice]++;
    info("CreateDevice adapter %u type %d focus %p behavior 0x%lx\n", adapter, type, (void *)focus, flags);
    log_pp("  pp", pp);
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9Ex_CreateDevice(INNER(IDirect3D9Ex, self), adapter, type, focus, flags, pp, &dev);
    info("  -> hr 0x%lx\n", hr);
    *out = SUCCEEDED(hr) ? (IDirect3DDevice9 *)wrap(dev_vtbl, (IUnknown *)dev) : NULL;
    return hr;
}

static HRESULT WINAPI d3d_CreateDeviceEx(IDirect3D9Ex *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                         D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode, IDirect3DDevice9Ex **out)
{
    d3d_calls[D3D_CreateDeviceEx]++;
    info("CreateDeviceEx adapter %u type %d focus %p behavior 0x%lx fsmode %p\n", adapter, type, (void *)focus, flags, (void *)mode);
    log_pp("  pp", pp);
    IDirect3DDevice9Ex *dev = NULL;
    HRESULT hr = IDirect3D9Ex_CreateDeviceEx(INNER(IDirect3D9Ex, self), adapter, type, focus, flags, pp, mode, &dev);
    info("  -> hr 0x%lx\n", hr);
    *out = SUCCEEDED(hr) ? (IDirect3DDevice9Ex *)wrap(dev_vtbl, (IUnknown *)dev) : NULL;
    return hr;
}

/* ---------------------------------------------------------------- timer probe
 * Every second: 20x Sleep(1), log mean/max actual duration. Characterises the OS timer behaviour that
 * Source's polling loops and fps_max limiter depend on (macOS timer coalescing under Wine). */
static DWORD WINAPI timer_probe(void *arg)
{
    for (;;) {
        LARGE_INTEGER a, b;
        double sum = 0, mx = 0;
        for (int i = 0; i < 20; i++) {
            QueryPerformanceCounter(&a); Sleep(1); QueryPerformanceCounter(&b);
            double d = qpc_ms(b.QuadPart - a.QuadPart);
            sum += d; if (d > mx) mx = d;
        }
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        info("SLEEP1 t=%.1f mean=%.2f max=%.2f frame=%llu\n", qpc_ms(now.QuadPart - t0.QuadPart) / 1000, sum / 20, mx,
             (unsigned long long)frame_no);
        Sleep(1000);
    }
    return 0;
}

/* ---------------------------------------------------------------- setup + exports */
static BOOL init_once(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    static BOOL ok;
    BOOL pending;
    InitOnceBeginInitialize(&once, 0, &pending, NULL);
    if (!pending) return ok;

    InitializeCriticalSection(&log_lock);
    log_open();
    WCHAR path[MAX_PATH];
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)init_once, &self);
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') n--;
    lstrcpyW(path + n, L"d3d9_ref.dll");
    ref_dll = LoadLibraryW(path);
    info("oracle d3d9_ref.dll: %p\n", (void *)ref_dll);

    memcpy(d3d_vtbl, d3d_thunks, sizeof d3d_vtbl);
    memcpy(dev_vtbl, dev_thunks, sizeof dev_vtbl);
    d3d_vtbl[D3D_Release] = d3d_Release;
    d3d_vtbl[D3D_GetAdapterIdentifier] = d3d_GetAdapterIdentifier;
    d3d_vtbl[D3D_CreateDevice] = d3d_CreateDevice;
    d3d_vtbl[D3D_CreateDeviceEx] = d3d_CreateDeviceEx;
    dev_vtbl[DEV_QueryInterface] = dev_QueryInterface;
    dev_vtbl[DEV_Release] = dev_Release;
    dev_vtbl[DEV_Present] = dev_Present;
    dev_vtbl[DEV_PresentEx] = dev_PresentEx;
    dev_vtbl[DEV_Reset] = dev_Reset;
    dev_vtbl[DEV_SetVertexShaderConstantF] = dev_SetVertexShaderConstantF;
    dev_vtbl[DEV_ResetEx] = dev_ResetEx;

    census_install(d3d_vtbl, dev_vtbl, dir, tag);
    capture_install(d3d_vtbl, dev_vtbl, dir, tag);
    info("mode: %s\n", census_enabled() ? "census" : capture_on ? "capture" : "timing");
    if (!census_enabled() && !capture_on) CloseHandle(CreateThread(NULL, 0, timer_probe, NULL, 0, NULL));
    ok = ref_dll != NULL;
    InitOnceComplete(&once, 0, NULL);
    return ok;
}

__declspec(dllexport) IDirect3D9 *WINAPI Direct3DCreate9(UINT sdk)
{
    if (!init_once()) return NULL;
    IDirect3D9 *(WINAPI *fn)(UINT) = (void *)GetProcAddress(ref_dll, "Direct3DCreate9");
    IDirect3D9 *d = fn(sdk);
    info("Direct3DCreate9(%u) -> %p\n", sdk, (void *)d);
    return d ? (IDirect3D9 *)wrap(d3d_vtbl, (IUnknown *)d) : NULL;
}

__declspec(dllexport) HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex **out)
{
    if (!init_once()) return E_FAIL;
    HRESULT (WINAPI *fn)(UINT, IDirect3D9Ex **) = (void *)GetProcAddress(ref_dll, "Direct3DCreate9Ex");
    IDirect3D9Ex *d = NULL;
    HRESULT hr = fn(sdk, &d);
    info("Direct3DCreate9Ex(%u) -> hr 0x%lx\n", sdk, hr);
    *out = SUCCEEDED(hr) ? (IDirect3D9Ex *)wrap(d3d_vtbl, (IUnknown *)d) : NULL;
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    if (reason == DLL_PROCESS_DETACH && out_csv != INVALID_HANDLE_VALUE) {
        flush_csv();
        census_dump();
        capture_close();
        dump_call_counts();
    }
    return TRUE;
}
