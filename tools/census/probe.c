/* probe — adapter parity probe (x86_64 PE, run under the Wine runtime).
 *   probe.exe <d3d9 dll (path or name)> <out.tsv> [queries.tsv]
 * Loads the given D3D9 provider (DXVK's d3d9.dll by path, or tf2mt.dll by name), asks it everything TF2 asks the
 * IDirect3D9 object (identity, caps, display modes, plus every census query from queries.tsv) and writes a canonical
 * TSV. Run it once per provider and `diff` the outputs: identical output == the substitution is faithful.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

static FILE *out;
#define P(...) fprintf(out, __VA_ARGS__)

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: probe <dll> <out.tsv> [queries.tsv]\n"); return 2; }
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { fprintf(stderr, "cannot load %s (err %lu)\n", argv[1], GetLastError()); return 1; }
    HRESULT (WINAPI *create)(UINT, IDirect3D9Ex **) = (void *)GetProcAddress(m, "Direct3DCreate9Ex");
    IDirect3D9Ex *d3d = NULL;
    if (!create || FAILED(create(D3D_SDK_VERSION, &d3d))) { fprintf(stderr, "Direct3DCreate9Ex failed\n"); return 1; }
    out = fopen(argv[2], "w");

    P("adapters\t%u\n", IDirect3D9Ex_GetAdapterCount(d3d));
    D3DADAPTER_IDENTIFIER9 id;
    HRESULT hr = IDirect3D9Ex_GetAdapterIdentifier(d3d, 0, 0, &id);
    P("identity.hr\t0x%08lx\nidentity.driver\t%s\nidentity.description\t%s\nidentity.devicename\t%s\n", hr, id.Driver, id.Description, id.DeviceName);
    P("identity.driverversion\t0x%llx\nidentity.vendor\t0x%lx\nidentity.device\t0x%lx\nidentity.subsys\t0x%lx\nidentity.revision\t%lu\nidentity.whql\t%lu\n",
      (unsigned long long)id.DriverVersion.QuadPart, id.VendorId, id.DeviceId, id.SubSysId, id.Revision, id.WHQLLevel);
    P("identity.guid\t");
    for (unsigned i = 0; i < 16; i++) P("%02x", ((unsigned char *)&id.DeviceIdentifier)[i]);
    P("\n");

    D3DCAPS9 caps;
    hr = IDirect3D9Ex_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps);
    P("caps.hr\t0x%08lx\n", hr);
    for (unsigned i = 0; i < sizeof caps; i += 16) {
        P("caps.%03x\t", i);
        for (unsigned k = i; k < i + 16 && k < sizeof caps; k++) P("%02x", ((unsigned char *)&caps)[k]);
        P("\n");
    }

    D3DDISPLAYMODE dm;
    hr = IDirect3D9Ex_GetAdapterDisplayMode(d3d, 0, &dm);
    P("displaymode\t0x%08lx\t%ux%u@%u fmt %d\n", hr, dm.Width, dm.Height, dm.RefreshRate, dm.Format);
    static const D3DFORMAT mode_fmts[] = { D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5, D3DFMT_A2R10G10B10 };
    for (unsigned f = 0; f < sizeof mode_fmts / sizeof *mode_fmts; f++) {
        UINT n = IDirect3D9Ex_GetAdapterModeCount(d3d, 0, mode_fmts[f]);
        P("modecount\t%d\t%u\n", mode_fmts[f], n);
        for (UINT i = 0; i < n; i++)
            if (SUCCEEDED(IDirect3D9Ex_EnumAdapterModes(d3d, 0, mode_fmts[f], i, &dm)))
                P("mode\t%d\t%u\t%ux%u@%u\n", mode_fmts[f], i, dm.Width, dm.Height, dm.RefreshRate);
    }

    if (argc > 3) {
        FILE *q = fopen(argv[3], "r");
        char line[512];
        unsigned bad = 0, total = 0;
        while (q && fgets(line, sizeof line, q)) {
            if (line[0] == '#') continue;
            unsigned a, b, c, d, e, f;
            HRESULT r; char kind[8];
            if (sscanf(line, "%7s", kind) != 1) continue;
            if (!strcmp(kind, "fmt") && sscanf(line, "fmt %u %u %u %u %u", &a, &b, &c, &d, &e) == 5)
                r = IDirect3D9Ex_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, d, c, b, a);
            else if (!strcmp(kind, "ds") && sscanf(line, "ds %u %u %u", &a, &b, &c) == 3)
                r = IDirect3D9Ex_CheckDepthStencilMatch(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, a, b);
            else if (!strcmp(kind, "type") && sscanf(line, "type %u %u %u %u %u", &a, &b, &c, &d, &e) == 5)
                r = IDirect3D9Ex_CheckDeviceType(d3d, 0, d, a, b, c);
            else continue;
            f = (r == D3D_OK);
            P("query\t%.*s\t%d\n", (int)strcspn(line, "\n"), line, f);
            total++; bad += 0;
        }
        (void)bad; (void)total;
        if (q) fclose(q);
    }
    /* MSAA (policy, not census) */
    static const D3DFORMAT ms_fmts[] = { D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D24S8, (D3DFORMAT)113, D3DFMT_R5G6B5 };
    for (unsigned f = 0; f < sizeof ms_fmts / sizeof *ms_fmts; f++)
        for (unsigned ms = 0; ms <= 16; ms++) {
            DWORD ql = 0;
            hr = IDirect3D9Ex_CheckDeviceMultiSampleType(d3d, 0, D3DDEVTYPE_HAL, ms_fmts[f], TRUE, (D3DMULTISAMPLE_TYPE)ms, &ql);
            P("msaa\t%d\t%u\t%d\n", ms_fmts[f], ms, hr == D3D_OK);
        }
    fclose(out);
    IDirect3D9Ex_Release(d3d);
    return 0;
}
