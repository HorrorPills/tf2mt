/* tf2mt command stream (M6): the frontend (tf2mt.dll) appends packets as the game calls the device; the backend
 * (tf2mt.so, src/unixlib/render.mm) decodes them in order and encodes Metal work. Flushed with TF2MT_UNIX_SUBMIT.
 * Packet = uint32 header (op in bits 0-7, payload length in dwords in bits 8-31) + payload dwords.
 * Object references are backend handles (unix_calls.h); surfaces are (handle, face, level) triples. */
#pragma once
#include <stdint.h>

enum tf2mt_cmd {
    TF2MT_CMD_RESET_STATE = 1,   /* (none) D3D9 default render/sampler state (CreateDevice, Reset) */
    TF2MT_CMD_RS,                /* state, value */
    TF2MT_CMD_SS,                /* sampler (0-15 PS, 16-19 = D3DVERTEXTEXTURESAMPLER0-3), type, value */
    TF2MT_CMD_TEXTURE,           /* stage (same numbering), handle */
    TF2MT_CMD_STREAM,            /* stream, handle, offset, stride */
    TF2MT_CMD_INDICES,           /* handle, D3DFORMAT */
    TF2MT_CMD_VDECL,             /* handle */
    TF2MT_CMD_VS,                /* handle */
    TF2MT_CMD_PS,                /* handle */
    TF2MT_CMD_VS_F,              /* start, count, count*4 floats */
    TF2MT_CMD_PS_F,
    TF2MT_CMD_VS_I,              /* start, count, count*4 ints */
    TF2MT_CMD_PS_I,
    TF2MT_CMD_VS_B,              /* start, count, count BOOLs */
    TF2MT_CMD_PS_B,
    TF2MT_CMD_RT,                /* index, handle, face, level */
    TF2MT_CMD_DS,                /* handle */
    TF2MT_CMD_VIEWPORT,          /* x, y, w, h (uint32), minz, maxz (float) */
    TF2MT_CMD_SCISSOR,           /* left, top, right, bottom */
    TF2MT_CMD_CLIP_PLANE,        /* index, 4 floats */
    TF2MT_CMD_CLEAR,             /* flags, color, z (float), stencil, nrects, nrects*4 int32 */
    TF2MT_CMD_DRAW_INDEXED,      /* primtype, basevertex (int32), minindex, numvertices, startindex, primcount */
    TF2MT_CMD_DRAW,              /* primtype, startvertex, primcount */
    TF2MT_CMD_STRETCH,           /* src handle, face, level, has_rect, l, t, r, b, dst handle, face, level, has_rect, l, t, r, b, filter */
    TF2MT_CMD_COLOR_FILL,        /* handle, face, level, has_rect, l, t, r, b, color */
    TF2MT_CMD_PRESENT,           /* back buffer handle */
    TF2MT_CMD_QUERY_BEGIN,       /* slot, gen: occlusion query, following draws count samples */
    TF2MT_CMD_QUERY_END,         /* slot, gen */
    TF2MT_CMD_EVENT,             /* slot, gen: event query, signalled when the GPU reaches it */
    TF2MT_CMD_DESTROY,           /* handle: release a backend object in stream order (M9 encoder thread) */
    TF2MT_CMD_BUFFER_SWITCH,     /* handle, token: make a renamed backing (TF2MT_UNIX_RENAME_BUFFER) current */
    TF2MT_CMD_COUNT
};

#define TF2MT_CMD_HEADER(op, ndw) ((uint32_t)(op) | (uint32_t)(ndw) << 8)
