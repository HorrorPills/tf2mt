/* tf2mt unix-call interface shared by the PE frontend (tf2mt.dll) and the unix library (tf2mt.so).
 * Parameter structs use fixed-size fields only: both sides are x86_64 but compiled by different compilers
 * (mingw-w64 vs Apple clang), so no pointers-to-PE-memory semantics beyond plain 64-bit integers. */
#pragma once
#include <stdint.h>

enum tf2mt_unix_call {
    TF2MT_UNIX_INIT,
    TF2MT_UNIX_ATTACH,
    TF2MT_UNIX_PRESENT,
    TF2MT_UNIX_DETACH,
    TF2MT_UNIX_QUERY_DEVICE,
    /* M5 resources (src/unixlib/resources.mm) */
    TF2MT_UNIX_CREATE_BUFFER,     /* struct tf2mt_buffer_params */
    TF2MT_UNIX_RENAME_BUFFER,     /* struct tf2mt_buffer_params: new backing for a DISCARD lock */
    TF2MT_UNIX_CREATE_TEXTURE,    /* struct tf2mt_texture_params */
    TF2MT_UNIX_DESTROY,           /* struct tf2mt_handle_params */
    TF2MT_UNIX_UPLOAD,            /* struct tf2mt_upload_params */
    TF2MT_UNIX_READBACK,          /* struct tf2mt_upload_params (dst = src field): synchronous texture -> CPU copy */
    TF2MT_UNIX_FLUSH,             /* struct tf2mt_handle_params (ignored): commit pending uploads */
    TF2MT_UNIX_STATS,             /* struct tf2mt_stats */
    /* M6 rendering (src/unixlib/render.mm) */
    TF2MT_UNIX_CREATE_SHADER,     /* struct tf2mt_shader_params */
    TF2MT_UNIX_CREATE_DECL,       /* struct tf2mt_decl_params */
    TF2MT_UNIX_SUBMIT,            /* struct tf2mt_submit_params: decode + encode a command stream (commands.h) */
    TF2MT_UNIX_QUERY_DATA,        /* struct tf2mt_query_params */
    TF2MT_UNIX_COUNT
};

struct tf2mt_init_params {
    char log_path[1024];       /* unix path, may be empty */
};

struct tf2mt_attach_params {
    uint64_t hwnd;             /* the game's HWND (Wine user handle value) */
    uint32_t width, height;
    uint32_t vsync;
};

struct tf2mt_present_params {
    float r, g, b;
};

struct tf2mt_device_info {
    char name[256];                    /* MTLDevice.name, e.g. "Apple M3 Pro" */
    uint64_t recommended_working_set;  /* bytes, MTLDevice.recommendedMaxWorkingSetSize (unified memory) */
    uint32_t max_msaa;                 /* highest supported sample count (1, 2, 4 or 8) */
    uint32_t msaa_mask;                /* bit n set = sample count n supported (bits 1, 2, 4, 8) */
};

/* ---- M5 resources. Handles are 32-bit generational ids (index bits 0-19, generation bits 20-31); 0 = none. */
struct tf2mt_buffer_params {
    uint32_t size;             /* in: bytes */
    uint32_t dynamic;          /* in: D3DUSAGE_DYNAMIC (renamed on DISCARD) */
    uint32_t handle;           /* in (rename) / out (create) */
    uint32_t status;           /* out: 0 ok, else D3DERR_OUTOFVIDEOMEMORY-style failure */
    uint64_t cpu;              /* out: CPU address of the new backing (shared storage, same process) */
    uint32_t token;            /* out (rename): pass to TF2MT_CMD_BUFFER_SWITCH; the switch happens in stream order */
    uint32_t pad;
};

enum tf2mt_tex_type { TF2MT_TEX_2D = 0, TF2MT_TEX_CUBE = 1, TF2MT_TEX_3D = 2 };
struct tf2mt_texture_params {
    uint32_t type;             /* enum tf2mt_tex_type */
    uint32_t d3dfmt;           /* D3DFORMAT */
    uint32_t width, height, depth, levels;
    uint32_t usage;            /* D3DUSAGE_* (RENDERTARGET, DEPTHSTENCIL, DYNAMIC) */
    uint32_t samples;          /* 1 = no MSAA */
    uint32_t handle;           /* out */
    uint32_t status;           /* out: 0 ok, 1 unsupported format, 2 allocation failed */
};

struct tf2mt_handle_params { uint32_t handle, pad; };

/* Copy a CPU region into (UPLOAD) or out of (READBACK) one subresource. Pixels; block formats in whole blocks
 * except where the level itself is smaller than a block. src is a CPU address in this process. */
struct tf2mt_upload_params {
    uint32_t handle, face, level, pad;
    uint32_t x, y, z, width, height, depth;
    uint32_t row_pitch, slice_pitch;
    uint64_t src;
    uint32_t status, pad2;
};

struct tf2mt_stats {
    uint64_t upload_bytes, upload_calls, upload_ns, readback_bytes;
    uint64_t buffers_live, textures_live, buffer_bytes_live, texture_bytes_live;
    uint64_t renames, rename_allocs, verify_ok, verify_fail;
};

/* ---- M6 */
struct tf2mt_shader_params {
    uint64_t code;             /* in: D3D9 token stream (CPU address) */
    uint32_t dwords;           /* in */
    uint32_t handle;           /* out */
    uint32_t status;           /* out: 0 ok, 1 decode/translate failed (logged; draws using it are skipped) */
    uint32_t pad;
};
struct tf2mt_decl_params {
    uint64_t elements;         /* in: D3DVERTEXELEMENT9 array incl. D3DDECL_END */
    uint32_t count;            /* in: elements incl. END */
    uint32_t handle;           /* out */
};
#define TF2MT_QUERY_SLOTS 16384
struct tf2mt_query_params {
    uint32_t slot;             /* in */
    uint32_t flush;            /* in: D3DGETDATA_FLUSH -> submit queued GPU work if the query is not committed yet */
    uint32_t done;             /* out */
    uint32_t gen;              /* in: issue generation (a slot whose last END/EVENT is another generation is not ready) */
    uint64_t value;            /* out: samples passed (occlusion) */
};
struct tf2mt_submit_params {
    uint64_t data;             /* in: command stream (CPU address) */
    uint32_t bytes;            /* in */
    uint32_t pad;
};
