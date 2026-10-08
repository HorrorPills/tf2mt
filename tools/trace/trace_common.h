/* shared between trace.c (timing proxy) and census.c (census mode) */
#pragma once
#include <stdint.h>
typedef struct { void **vtbl; IUnknown *inner; } wrap_t;
#define INNER(T, w) ((T *)((wrap_t *)(w))->inner)

extern void *d3d_thunks[D3D_NMETHODS], *dev_thunks[DEV_NMETHODS];
extern void *d3d_cthunks[D3D_NMETHODS], *dev_cthunks[DEV_NMETHODS];
extern uint64_t d3d_calls[D3D_NMETHODS], dev_calls[DEV_NMETHODS];

void info(const char *fmt, ...);
void census_hit(DWORD code);
void census_install(void **d3d_vt, void **dev_vt, const char *dir, const char *tag);
void census_frame(uint64_t draws);
void census_dump(void);
int census_enabled(void);

/* capture mode (capture.c) */
extern int capture_on;
void capture_install(void **d3d_vt, void **dev_vt, const char *dir, const char *tag);
void capture_hit(DWORD code);
void capture_close(void);
