/* .t9 capture format — a D3D9 call stream recorded by tools/trace (TF2MT_TRACE_MODE=capture) and replayed by
 * tools/replay against any D3D9 provider (DXVK = oracle, tf2mt = under test). PLAN.md §7, §11.
 *
 * File: t9_header, then records { uint32 op; uint32 size; uint8 payload[size]; } back to back, in the exact order
 * the calls reached the device (the capture serialises all device and resource calls under one lock).
 * Objects are referenced by 32-bit ids assigned by the capture (0 = NULL); every id is introduced by a CREATE_* or
 * GET_* record before use. Variable-length data (shader code, constants, lock contents) follows the fixed struct
 * inside the same record. All integers little endian; structs are naturally aligned 4-byte fields (no pointers).
 * Lock contents are stored tightly packed (row bytes = width in blocks × block size), so replay works for any pitch.
 */
#pragma once
#include <stdint.h>

#define T9_MAGIC 0x31433954u   /* "T9C1" */
#define T9_VERSION 1

typedef struct { uint32_t magic, version, flags, reserved; } t9_header;

enum t9_op {
    /* device lifetime / frame */
    T9_CREATE_DEVICE = 1,   /* t9_create_device; device id is always 1 */
    T9_RESET,               /* t9_pp */
    T9_PRESENT,             /* (no payload) */
    T9_BEGIN_SCENE,
    T9_END_SCENE,
    T9_CLEAR,               /* t9_clear + count × D3DRECT(4 × int32) */
    /* state */
    T9_SET_RENDER_STATE,    /* u32 state, u32 value */
    T9_SET_SAMPLER_STATE,   /* u32 sampler, u32 type, u32 value */
    T9_SET_TSS,             /* u32 stage, u32 type, u32 value */
    T9_SET_TEXTURE,         /* u32 stage, u32 id */
    T9_SET_STREAM_SOURCE,   /* u32 stream, u32 id, u32 offset, u32 stride */
    T9_SET_STREAM_FREQ,     /* u32 stream, u32 setting */
    T9_SET_INDICES,         /* u32 id */
    T9_SET_VDECL,           /* u32 id */
    T9_SET_FVF,             /* u32 fvf */
    T9_SET_VS,              /* u32 id */
    T9_SET_PS,              /* u32 id */
    T9_VS_CONST_F,          /* u32 start, u32 count, count × 4 floats */
    T9_VS_CONST_I,          /* u32 start, u32 count, count × 4 int32 */
    T9_VS_CONST_B,          /* u32 start, u32 count, count × BOOL(int32) */
    T9_PS_CONST_F,
    T9_PS_CONST_I,
    T9_PS_CONST_B,
    T9_SET_RT,              /* u32 index, u32 id */
    T9_SET_DS,              /* u32 id */
    T9_SET_VIEWPORT,        /* D3DVIEWPORT9 (6 × 4 bytes) */
    T9_SET_SCISSOR,         /* RECT (4 × int32) */
    T9_SET_CLIP_PLANE,      /* u32 index, 4 floats */
    T9_SET_TRANSFORM,       /* u32 state, 16 floats (vestigial fixed function) */
    T9_SET_MATERIAL,        /* D3DMATERIAL9 (17 floats) */
    T9_SET_GAMMA_RAMP,      /* u32 swapchain, u32 flags, D3DGAMMARAMP (3 × 256 × u16) */
    /* draws */
    T9_DRAW,                /* u32 type, u32 start, u32 count */
    T9_DRAW_INDEXED,        /* u32 type, i32 base, u32 min, u32 nverts, u32 start, u32 count */
    T9_DRAW_UP,             /* u32 type, u32 count, u32 stride, u32 bytes, data */
    T9_DRAW_INDEXED_UP,     /* u32 type, u32 min, u32 nverts, u32 count, u32 ifmt, u32 vstride, u32 ibytes, u32 vbytes, idata, vdata */
    /* copies */
    T9_STRETCH_RECT,        /* u32 src, u32 has_srect, RECT, u32 dst, u32 has_drect, RECT, u32 filter */
    T9_UPDATE_SURFACE,      /* u32 src, u32 has_rect, RECT, u32 dst, u32 has_point, POINT */
    T9_UPDATE_TEXTURE,      /* u32 src, u32 dst */
    T9_GET_RT_DATA,         /* u32 rt, u32 dst */
    T9_COLOR_FILL,          /* u32 surface, u32 has_rect, RECT, u32 color */
    /* object creation and lookup (first u32 of every payload is the new id) */
    T9_CREATE_TEXTURE,      /* id, w, h, levels, usage, fmt, pool */
    T9_CREATE_CUBE,         /* id, edge, levels, usage, fmt, pool */
    T9_CREATE_VOLUME,       /* id, w, h, d, levels, usage, fmt, pool */
    T9_CREATE_VB,           /* id, length, usage, fvf, pool */
    T9_CREATE_IB,           /* id, length, usage, fmt, pool */
    T9_CREATE_RT,           /* id, w, h, fmt, ms, quality, lockable */
    T9_CREATE_DS,           /* id, w, h, fmt, ms, quality, discard */
    T9_CREATE_OFFSCREEN,    /* id, w, h, fmt, pool */
    T9_CREATE_QUERY,        /* id, type */
    T9_CREATE_VS,           /* id, ndwords, code */
    T9_CREATE_PS,           /* id, ndwords, code */
    T9_CREATE_VDECL,        /* id, nelements (incl. END), elements (D3DVERTEXELEMENT9, 8 bytes each) */
    T9_GET_SURFACE_LEVEL,   /* id, texture, level */
    T9_GET_CUBE_SURFACE,    /* id, cube, face, level */
    T9_GET_RT,              /* id, index */
    T9_GET_DS,              /* id */
    T9_GET_BACKBUFFER,      /* id, swapchain, index, type */
    /* object calls */
    T9_ADDREF,              /* u32 id */
    T9_RELEASE,             /* u32 id, u32 refcount the game saw */
    T9_BUFFER_WRITE,        /* u32 id, u32 offset, u32 size, u32 flags, data[size]   (VB/IB Lock+Unlock) */
    T9_SURFACE_WRITE,       /* t9_rect_write + packed rows   (surface LockRect/UnlockRect; texture level / cube face via GET_*) */
    T9_TEXTURE_WRITE,       /* u32 id, u32 face(cube, else 0), u32 level, then t9_rect_write + rows (texture LockRect) */
    T9_VOLUME_WRITE,        /* t9_box_write + packed slices */
    T9_QUERY_ISSUE,         /* u32 id, u32 flags */
    T9_QUERY_GETDATA,       /* u32 id, u32 size, u32 flags, u32 hr the game got */
    T9_GEN_MIPS,            /* u32 texture id */
    T9_EVICT_MANAGED,
    T9_LOCK_READ,           /* u32 id, u32 kind (0 buffer, 1 surface): READONLY lock, no data (readback) */
    T9_NOTE,                /* free text (diagnostics) */
    T9_OP_COUNT
};

typedef struct {
    uint32_t BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount, MultiSampleType, MultiSampleQuality,
             SwapEffect, Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat, Flags, FullScreen_RefreshRateInHz,
             PresentationInterval;
} t9_pp;

typedef struct { uint32_t adapter, devtype, behavior, ex; t9_pp pp; } t9_create_device;

typedef struct { uint32_t count, flags, color; float z; uint32_t stencil; } t9_clear;

/* Locked region of a surface / texture level: rect in pixels; rows × row_bytes of packed data follow. */
typedef struct {
    uint32_t id, flags, has_rect;
    int32_t left, top, right, bottom;   /* effective rect (whole surface when has_rect == 0) */
    uint32_t format, row_bytes, rows;
} t9_rect_write;

typedef struct {
    uint32_t id, level, flags, has_box;
    uint32_t left, top, right, bottom, front, back;
    uint32_t format, row_bytes, rows, slices;
} t9_box_write;

/* Block geometry of the formats TF2 locks (census "Locks: surfaces/volume"); 0 = unknown format. */
static inline uint32_t t9_block_bytes(uint32_t fmt, uint32_t *block_dim)
{
    *block_dim = 1;
    switch (fmt) {
    case 0x31545844u: *block_dim = 4; return 8;    /* DXT1 */
    case 0x32545844u: case 0x33545844u: case 0x34545844u: case 0x35545844u: *block_dim = 4; return 16;  /* DXT2-5 */
    case 0x31495441u: *block_dim = 4; return 8;    /* ATI1 */
    case 0x32495441u: *block_dim = 4; return 16;   /* ATI2 */
    case 20: return 3;                              /* R8G8B8 */
    case 21: case 22: case 31: case 32: case 33: case 34: case 35: case 62: case 63: case 64: case 67: case 112: case 114:
        return 4;
    case 23: case 24: case 25: case 26: case 29: case 30: case 40: case 51: case 60: case 61: case 81: case 111: return 2;
    case 27: case 28: case 41: case 50: case 52: return 1;
    case 36: case 110: case 113: case 115: return 8;  /* A16B16G16R16, Q16W16V16U16, A16B16G16R16F, G32R32F */
    case 116: return 16;                           /* A32B32G32R32F */
    default: return 0;
    }
}
