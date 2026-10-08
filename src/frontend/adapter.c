/* src/frontend/adapter.c — tf2mt's own IDirect3D9Ex (M3). No oracle: every answer comes from
 *   - caps_table.h (generated from census runs by tools/census/gen_caps.py): identity, D3DCAPS9, format/depth/device-type
 *     tables — a combination is OK only if TF2 asked for it in a census run and the reference (DXVK) answered OK;
 *   - the real display (Wine's EnumDisplaySettings) for modes;
 *   - the real Metal device (unix call) for the adapter description and MSAA sample counts.
 * Queries that are not in the census are answered NOTAVAILABLE and logged once ("UNSEEN ...") so the next census pass
 * can extend the tables deliberately (PLAN D9). */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "methods.h"
#include "ifaces.h"
#include "caps_table.h"
#include "../common/unix_calls.h"

_Static_assert(sizeof(D3DCAPS9) == 304, "D3DCAPS9 layout differs from the captured struct");
_Static_assert(sizeof(D3DADAPTER_IDENTIFIER9) == 1104, "D3DADAPTER_IDENTIFIER9 layout differs from the captured struct");

/* provided by device.c */
void logmsg(const char *fmt, ...);
NTSTATUS tf2mt_unix_call(int code, void *args);
HRESULT tf2mt_create_device(IDirect3D9Ex *d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                            D3DPRESENT_PARAMETERS *pp, void **out);

typedef struct { void **vtbl; volatile LONG ref; } d3d_t;
static void *vt[M_IDirect3D9Ex_N];

#define FMT_X8R8G8B8 22u
#define FMT_A8R8G8B8 21u

/* ---------------------------------------------------------------- Metal device info (lazy) */
static struct tf2mt_device_info dinfo;
static int dinfo_state;   /* 0 = not queried, 1 = ok, -1 = failed */
static void ensure_dinfo(void)
{
    if (dinfo_state) return;
    dinfo_state = tf2mt_unix_call(TF2MT_UNIX_QUERY_DEVICE, &dinfo) == 0 ? 1 : -1;
    if (dinfo_state > 0)
        logmsg("adapter: Metal device '%s', msaa mask 0x%x, working set %llu MB\n", dinfo.name, dinfo.msaa_mask,
               (unsigned long long)(dinfo.recommended_working_set >> 20));
}

/* ---------------------------------------------------------------- "UNSEEN" log (deduplicated) */
static void unseen(const char *what, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    static uint64_t seen[256]; static int n;
    uint64_t h = 1469598103934665603ull;
    for (const char *p = what; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    uint32_t v[4] = { a, b, c, d };
    for (int i = 0; i < 4; i++) { h ^= v[i]; h *= 1099511628211ull; }
    for (int i = 0; i < n; i++) if (seen[i] == h) return;
    if (n < 256) seen[n++] = h;
    logmsg("UNSEEN %s %#x %#x %#x %#x -> NOTAVAILABLE (no OK row in census; may also be a recorded NO)\n", what, a, b, c, d);
}

/* ---------------------------------------------------------------- display modes (X8R8G8B8 only: all TF2 enumerates) */
typedef struct { UINT w, h, hz; } mode_t_;
static mode_t_ modes[512]; static UINT nmodes; static int modes_done;
static int cmp_mode(const void *x, const void *y)
{
    const mode_t_ *a = x, *b = y;
    if (a->w != b->w) return a->w < b->w ? -1 : 1;
    if (a->h != b->h) return a->h < b->h ? -1 : 1;
    return a->hz < b->hz ? -1 : a->hz > b->hz;
}
static void load_modes(void)
{
    if (modes_done) return;
    modes_done = 1;
    DEVMODEW dm;
    for (DWORD i = 0; nmodes < 512; i++) {
        memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (!EnumDisplaySettingsW(NULL, i, &dm)) break;
        if (dm.dmBitsPerPel != 32 || dm.dmDisplayFrequency < 2) continue;
        int dup = 0;
        for (UINT k = 0; k < nmodes; k++)
            if (modes[k].w == dm.dmPelsWidth && modes[k].h == dm.dmPelsHeight && modes[k].hz == dm.dmDisplayFrequency) { dup = 1; break; }
        if (!dup) modes[nmodes++] = (mode_t_){ dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency };
    }
    qsort(modes, nmodes, sizeof *modes, cmp_mode);
    logmsg("adapter: %u display modes (X8R8G8B8)\n", nmodes);
}

/* ---------------------------------------------------------------- IUnknown */
static HRESULT WINAPI d3d_QueryInterface(d3d_t *self, REFIID riid, void **out)
{
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IDirect3D9) || IsEqualGUID(riid, &IID_IDirect3D9Ex)) {
        InterlockedIncrement(&self->ref); *out = self; return D3D_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI d3d_AddRef(d3d_t *self) { return (ULONG)InterlockedIncrement(&self->ref); }
static ULONG WINAPI d3d_Release(d3d_t *self)
{
    LONG r = InterlockedDecrement(&self->ref);
    if (!r) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)r;
}

/* ---------------------------------------------------------------- adapter */
static UINT WINAPI d3d_GetAdapterCount(d3d_t *self) { return 1; }

static HRESULT WINAPI d3d_GetAdapterIdentifier(d3d_t *self, UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id)
{
    if (adapter) return D3DERR_INVALIDCALL;
    ensure_dinfo();
    memset(id, 0, sizeof *id);
    strncpy(id->Driver, TF2MT_ID_DRIVER, sizeof id->Driver - 1);
    strncpy(id->Description, dinfo_state > 0 ? dinfo.name : TF2MT_ID_DESCRIPTION_FALLBACK, sizeof id->Description - 1);
    strncpy(id->DeviceName, TF2MT_ID_DEVICENAME, sizeof id->DeviceName - 1);
    id->DriverVersion.QuadPart = TF2MT_ID_DRIVERVERSION;
    id->VendorId = TF2MT_ID_VENDOR; id->DeviceId = TF2MT_ID_DEVICE; id->SubSysId = TF2MT_ID_SUBSYS; id->Revision = TF2MT_ID_REVISION;
    memcpy(&id->DeviceIdentifier, TF2MT_ID_GUID, sizeof TF2MT_ID_GUID);
    id->WHQLLevel = TF2MT_ID_WHQL;
    return D3D_OK;
}

static UINT WINAPI d3d_GetAdapterModeCount(d3d_t *self, UINT adapter, D3DFORMAT fmt)
{
    if (adapter) return 0;
    if (fmt != FMT_X8R8G8B8) { unseen("GetAdapterModeCount", fmt, 0, 0, 0); return 0; }
    load_modes();
    return nmodes;
}

static HRESULT WINAPI d3d_EnumAdapterModes(d3d_t *self, UINT adapter, D3DFORMAT fmt, UINT idx, D3DDISPLAYMODE *m)
{
    if (adapter) return D3DERR_INVALIDCALL;
    if (fmt != FMT_X8R8G8B8) { unseen("EnumAdapterModes", fmt, 0, 0, 0); return D3DERR_INVALIDCALL; }
    load_modes();
    if (idx >= nmodes) return D3DERR_INVALIDCALL;
    m->Width = modes[idx].w; m->Height = modes[idx].h; m->RefreshRate = modes[idx].hz; m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT tf2mt_adapter_display_mode(D3DDISPLAYMODE *m)
{
    DEVMODEW dm = { .dmSize = sizeof dm };
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) return D3DERR_INVALIDCALL;
    m->Width = dm.dmPelsWidth; m->Height = dm.dmPelsHeight; m->RefreshRate = dm.dmDisplayFrequency; m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT WINAPI d3d_GetAdapterDisplayMode(d3d_t *self, UINT adapter, D3DDISPLAYMODE *m)
{
    return adapter ? D3DERR_INVALIDCALL : tf2mt_adapter_display_mode(m);
}

static HMONITOR WINAPI d3d_GetAdapterMonitor(d3d_t *self, UINT adapter)
{
    return adapter ? NULL : MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
}

HRESULT tf2mt_adapter_caps(D3DCAPS9 *caps)
{
    memcpy(caps, TF2MT_CAPS, sizeof *caps);
    return D3D_OK;
}
static HRESULT WINAPI d3d_GetDeviceCaps(d3d_t *self, UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps)
{
    if (adapter) return D3DERR_INVALIDCALL;
    return tf2mt_adapter_caps(caps);
}

/* ---------------------------------------------------------------- format / type / depth / MSAA answers */
static HRESULT WINAPI d3d_CheckDeviceType(d3d_t *self, UINT adapter, D3DDEVTYPE type, D3DFORMAT af, D3DFORMAT bf, BOOL windowed)
{
    if (adapter) return D3DERR_INVALIDCALL;
    for (size_t i = 0; i < sizeof TF2MT_TYPE_OK / sizeof *TF2MT_TYPE_OK; i++)
        if (TF2MT_TYPE_OK[i].adapter == af && TF2MT_TYPE_OK[i].backbuffer == bf && TF2MT_TYPE_OK[i].windowed == (uint32_t)!!windowed &&
            TF2MT_TYPE_OK[i].devtype == (uint32_t)type)
            return D3D_OK;
    /* Fullscreen (not in the census: the owner always ran windowed). D3D9 rule for the only display format we
     * enumerate (X8R8G8B8): X8R8G8B8 or A8R8G8B8 back buffers. TF2 asks this before creating a fullscreen device
     * and quits if nothing is available (2026-10-08). On macOS "fullscreen" is a borderless window over the display. */
    if (!windowed && type == D3DDEVTYPE_HAL && af == FMT_X8R8G8B8 && (bf == FMT_X8R8G8B8 || bf == D3DFMT_A8R8G8B8))
        return D3D_OK;
    unseen("CheckDeviceType", af, bf, windowed, type);
    return D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_CheckDeviceFormat(d3d_t *self, UINT adapter, D3DDEVTYPE type, D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT cf)
{
    if (adapter) return D3DERR_INVALIDCALL;
    if (af == FMT_X8R8G8B8)
        for (size_t i = 0; i < sizeof TF2MT_FORMAT_OK / sizeof *TF2MT_FORMAT_OK; i++)
            if (TF2MT_FORMAT_OK[i].fmt == (uint32_t)cf && TF2MT_FORMAT_OK[i].rtype == (uint32_t)rt && TF2MT_FORMAT_OK[i].usage == usage)
                return D3D_OK;
    unseen("CheckDeviceFormat", cf, rt, usage, af);   /* also logs real NO rows from the census: harmless duplicates */
    return D3DERR_NOTAVAILABLE;
}

static HRESULT WINAPI d3d_CheckDepthStencilMatch(d3d_t *self, UINT adapter, D3DDEVTYPE type, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df)
{
    if (adapter) return D3DERR_INVALIDCALL;
    for (size_t i = 0; i < sizeof TF2MT_DSMATCH_OK / sizeof *TF2MT_DSMATCH_OK; i++)
        if (TF2MT_DSMATCH_OK[i].rt == (uint32_t)rf && TF2MT_DSMATCH_OK[i].ds == (uint32_t)df) return D3D_OK;
    unseen("CheckDepthStencilMatch", rf, df, 0, 0);
    return D3DERR_NOTAVAILABLE;
}

/* MSAA is policy, not census: Metal sample counts come from the device (Apple GPUs: 2/4/8). Applies to the colour/depth
 * formats TF2 renders into (A8R8G8B8, X8R8G8B8, D24S8, A16B16G16R16F). */
static HRESULT WINAPI d3d_CheckDeviceMultiSampleType(d3d_t *self, UINT adapter, D3DDEVTYPE type, D3DFORMAT fmt, BOOL windowed,
                                                     D3DMULTISAMPLE_TYPE ms, DWORD *quality)
{
    if (adapter) return D3DERR_INVALIDCALL;
    ensure_dinfo();
    int fmt_ok = fmt == 21 || fmt == 22 || fmt == 75 || fmt == 113;
    if (ms == D3DMULTISAMPLE_NONE) { if (quality) *quality = 1; return D3D_OK; }
    if (fmt_ok && (ms == 2 || ms == 4 || ms == 8) && dinfo_state > 0 && (dinfo.msaa_mask & (uint32_t)ms)) {
        if (quality) *quality = 1;
        return D3D_OK;
    }
    unseen("CheckDeviceMultiSampleType", fmt, ms, windowed, 0);
    return D3DERR_NOTAVAILABLE;
}

/* ---------------------------------------------------------------- device creation */
static HRESULT WINAPI d3d_CreateDevice(d3d_t *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                       D3DPRESENT_PARAMETERS *pp, void **out)
{
    if (adapter) return D3DERR_INVALIDCALL;
    return tf2mt_create_device((IDirect3D9Ex *)self, adapter, type, focus, flags, pp, out);
}
static HRESULT WINAPI d3d_CreateDeviceEx(d3d_t *self, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                                         D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *mode, void **out)
{
    if (adapter) return D3DERR_INVALIDCALL;
    return tf2mt_create_device((IDirect3D9Ex *)self, adapter, type, focus, flags, pp, out);
}

/* ---------------------------------------------------------------- factory */
#define SET(m, fn) vt[M_IDirect3D9Ex_##m] = (void *)(fn)
IDirect3D9Ex *tf2mt_adapter_create(void)
{
    static int init;
    if (!init) {
        memcpy(vt, stubs_IDirect3D9Ex, sizeof vt);   /* anything not implemented below logs "UNIMPL" once */
        SET(QueryInterface, d3d_QueryInterface); SET(AddRef, d3d_AddRef); SET(Release, d3d_Release);
        SET(GetAdapterCount, d3d_GetAdapterCount); SET(GetAdapterIdentifier, d3d_GetAdapterIdentifier);
        SET(GetAdapterModeCount, d3d_GetAdapterModeCount); SET(EnumAdapterModes, d3d_EnumAdapterModes);
        SET(GetAdapterDisplayMode, d3d_GetAdapterDisplayMode); SET(GetAdapterMonitor, d3d_GetAdapterMonitor);
        SET(CheckDeviceType, d3d_CheckDeviceType); SET(CheckDeviceFormat, d3d_CheckDeviceFormat);
        SET(CheckDeviceMultiSampleType, d3d_CheckDeviceMultiSampleType); SET(CheckDepthStencilMatch, d3d_CheckDepthStencilMatch);
        SET(GetDeviceCaps, d3d_GetDeviceCaps); SET(CreateDevice, d3d_CreateDevice); SET(CreateDeviceEx, d3d_CreateDeviceEx);
        init = 1;
    }
    d3d_t *d = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *d);
    d->vtbl = vt; d->ref = 1;
    return (IDirect3D9Ex *)d;
}
