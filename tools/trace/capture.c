/* tools/trace capture mode (PLAN.md §7/§11): records the complete D3D9 call stream TF2 issues into a .t9 file
 * (format: t9.h) for tools/replay. Enabled with TF2MT_TRACE_MODE=capture; output %TF2MT_TRACE_DIR%\capture-<tag>.t9.
 *
 * Device / IDirect3D9 calls are intercepted by replacing slots of our wrapper vtables (chaining to whatever was there:
 * trace.c's timing hooks or the forwarding thunks). Resource-object calls (Lock/Unlock, AddRef/Release,
 * GetSurfaceLevel, query Issue/GetData…) are intercepted by patching the oracle's (DXVK's) resource vtables
 * in-process, exactly like census mode. The game is never touched.
 *
 * Ordering: every hook takes one recursive lock for its whole duration, so records appear in the order the calls
 * reached the device. Calls nested inside a hook (the oracle calling its own objects) and calls from threads that
 * never entered a device method (the oracle's worker threads) are not recorded.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include "methods.h"
#include "ifaces.h"
#include "trace_common.h"
#include "t9.h"

int capture_on;
static CRITICAL_SECTION cap_lock;
static HANDLE out = INVALID_HANDLE_VALUE;
static uint8_t *buf;
static size_t buf_len;
#define BUF_SIZE (32u << 20)
static uint64_t bytes_total, records_total, frames;
static DWORD tls_depth = TLS_OUT_OF_INDEXES;   /* per thread: (hook depth << 1) | game-thread flag */

/* ---------------------------------------------------------------- writer */
static void flush_out(void)
{
    DWORD w;
    size_t off = 0;
    while (off < buf_len && WriteFile(out, buf + off, (DWORD)(buf_len - off), &w, NULL) && w) off += w;
    buf_len = 0;
}

static void put(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        if (buf_len == BUF_SIZE) flush_out();
        size_t k = BUF_SIZE - buf_len < n ? BUF_SIZE - buf_len : n;
        memcpy(buf + buf_len, b, k);
        buf_len += k; b += k; n -= k;
    }
}

/* one record: header + up to three payload parts */
static void rec3(uint32_t op, const void *a, uint32_t na, const void *b, uint32_t nb, const void *c, uint32_t nc)
{
    uint32_t h[2] = {op, na + nb + nc};
    put(h, 8);
    if (na) put(a, na);
    if (nb) put(b, nb);
    if (nc) put(c, nc);
    bytes_total += 8 + h[1];
    records_total++;
}
#define REC(op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; rec3(op, a_, sizeof a_, NULL, 0, NULL, 0); } while (0)

/* ---------------------------------------------------------------- hook scope (lock + recursion/thread filter) */
static int enter(void)
{
    EnterCriticalSection(&cap_lock);
    uintptr_t d = (uintptr_t)TlsGetValue(tls_depth);
    TlsSetValue(tls_depth, (void *)((d + 2) | 1));   /* a thread that calls the device is a game thread */
    return d < 2;          /* outermost: record */
}
/* Resource hooks: 1 = record (lock held), 0 = nested call on a game thread (lock held, not recorded),
 * -1 = a thread that never called the device (the oracle's workers): no lock at all. Those threads may hold the
 * oracle's internal locks while calling AddRef/Release; taking our lock there deadlocked HDR map loads. */
static int enter_resource(void)
{
    uintptr_t d = (uintptr_t)TlsGetValue(tls_depth);
    if (!(d & 1)) return -1;
    EnterCriticalSection(&cap_lock);
    TlsSetValue(tls_depth, (void *)(d + 2));
    return d == 1;
}
static void leave_r(int r) { if (r >= 0) { uintptr_t d = (uintptr_t)TlsGetValue(tls_depth); TlsSetValue(tls_depth, (void *)(d - 2)); LeaveCriticalSection(&cap_lock); } }
static void leave(void)
{
    uintptr_t d = (uintptr_t)TlsGetValue(tls_depth);
    TlsSetValue(tls_depth, (void *)(d - 2));          /* keeps the game-thread bit */
    LeaveCriticalSection(&cap_lock);
}

/* ---------------------------------------------------------------- object ids */
enum { K_NONE, K_TEX, K_CUBE, K_VOL, K_VB, K_IB, K_SURF, K_QUERY, K_VS, K_PS, K_DECL };
typedef struct { void *ptr; uint32_t id, kind, parent, sub; } obj_t;
#define OBJ_BITS 21
#define OBJ_SIZE (1u << OBJ_BITS)
static obj_t *objs;
static uint32_t next_id = 2;            /* 1 = the device */
#define TOMB ((void *)1)

static uint32_t hash_ptr(const void *p) { uint64_t x = (uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (uint32_t)x; }

static obj_t *find(const void *p)
{
    if (!p) return NULL;
    for (uint32_t i = hash_ptr(p) & (OBJ_SIZE - 1);; i = (i + 1) & (OBJ_SIZE - 1)) {
        if (!objs[i].ptr) return NULL;
        if (objs[i].ptr == p) return &objs[i];
    }
}
static uint32_t id_of(const void *p) { obj_t *o = find(p); return o ? o->id : 0; }

static uint32_t add(void *p, uint32_t kind, uint32_t parent, uint32_t sub)
{
    obj_t *o = find(p);
    if (!o) {
        uint32_t i = hash_ptr(p) & (OBJ_SIZE - 1);
        while (objs[i].ptr && objs[i].ptr != TOMB) i = (i + 1) & (OBJ_SIZE - 1);
        o = &objs[i];
        o->ptr = p;
    }
    o->id = next_id++; o->kind = kind; o->parent = parent; o->sub = sub;
    return o->id;
}
static void forget(void *p) { obj_t *o = find(p); if (o) o->ptr = TOMB; }

/* getters: reuse the id when the pointer is already known for the same (parent, sub) */
static uint32_t got(void *p, uint32_t kind, uint32_t parent, uint32_t sub, int *is_new)
{
    obj_t *o = find(p);
    if (o && o->kind == kind && o->parent == parent && o->sub == sub) { *is_new = 0; return o->id; }
    *is_new = 1;
    return add(p, kind, parent, sub);
}

/* ---------------------------------------------------------------- resource vtable patching (per-vtable originals) */
typedef struct { void **vtbl; int slot; void *orig; } patch_t;
#define MAXPATCH 256
static patch_t patches[MAXPATCH];
static int npatches;

static void patch(void *obj, int slot, void *hook)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < npatches; i++) if (patches[i].vtbl == vt && patches[i].slot == slot) return;
    if (npatches == MAXPATCH || vt[slot] == hook) return;
    patches[npatches].vtbl = vt; patches[npatches].slot = slot; patches[npatches].orig = vt[slot];
    npatches++;
    DWORD old;
    VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old);
    vt[slot] = hook;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
}
static void *orig(void *obj, int slot)
{
    void **vt = *(void ***)obj;
    for (int i = 0; i < npatches; i++) if (patches[i].vtbl == vt && patches[i].slot == slot) return patches[i].orig;
    return NULL;   /* unreachable: a hook only runs on a patched vtable */
}
#define ORIG(T, obj, iface, m) ((T)orig(obj, M_##iface##_##m))

/* outstanding locks: key (object, sub) -> where the game writes */
typedef struct lock_s { void *obj; uint32_t sub; uint32_t id, flags, offset, size, has_rect; RECT r; D3DBOX box; uint32_t fmt, w, h, d;
                 uint8_t *ptr; int32_t pitch, slice; } lock_t;
#define MAXLOCK 64
static lock_t locks[MAXLOCK];
static lock_t *lock_slot(void *obj, uint32_t sub, int create)
{
    lock_t *free_ = NULL;
    for (int i = 0; i < MAXLOCK; i++) {
        if (locks[i].obj == obj && locks[i].sub == sub) return &locks[i];
        if (!locks[i].obj && !free_) free_ = &locks[i];
    }
    if (create && free_) { memset(free_, 0, sizeof *free_); free_->obj = obj; free_->sub = sub; return free_; }
    return NULL;
}

static void note(const char *fmt, ...)
{
    char b[256];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) rec3(T9_NOTE, b, (uint32_t)(n < (int)sizeof b ? n : (int)sizeof b - 1), NULL, 0, NULL, 0);
    info("capture: %s\n", b);
}

/* packed rows of a locked rect -> T9_SURFACE_WRITE / T9_TEXTURE_WRITE */
static void emit_rect(uint32_t op, const uint32_t *pre, uint32_t npre, struct lock_s *l)
{
    uint32_t bdim, bb = t9_block_bytes(l->fmt, &bdim);
    t9_rect_write rw = {l->id, l->flags, l->has_rect, l->r.left, l->r.top, l->r.right, l->r.bottom, l->fmt, 0, 0};
    if (!bb) { note("lock of unknown format %u (id %u): contents not captured", l->fmt, l->id); rec3(op, pre, npre * 4, &rw, sizeof rw, NULL, 0); return; }
    uint32_t wb = ((uint32_t)(l->r.right - l->r.left) + bdim - 1) / bdim, hb = ((uint32_t)(l->r.bottom - l->r.top) + bdim - 1) / bdim;
    rw.row_bytes = wb * bb; rw.rows = hb;
    uint32_t h[2] = {op, npre * 4 + (uint32_t)sizeof rw + rw.row_bytes * rw.rows};
    put(h, 8);
    if (npre) put(pre, npre * 4);
    put(&rw, sizeof rw);
    for (uint32_t y = 0; y < hb; y++) put(l->ptr + (size_t)y * (size_t)l->pitch, rw.row_bytes);
    bytes_total += 8 + h[1]; records_total++;
}

/* ---------------------------------------------------------------- D3D9Ex user-memory sysmem resources
 * A SYSTEMMEM texture/surface created with a non-NULL *pSharedHandle uses application memory that is written without
 * LockRect (TF2: every no-mip texture). Its contents are recorded right before each UpdateSurface/UpdateTexture that
 * reads it, as a TEXTURE_WRITE (texture level 0) or SURFACE_WRITE (plain surface) of the whole surface. */
typedef struct { uint32_t id; uint8_t *mem; uint32_t fmt, w, h, is_tex; } usermem_t;
#define MAXUSER 65536
static usermem_t usermem[MAXUSER];
static uint32_t nuser;
static void usermem_add(uint32_t id, void *mem, uint32_t fmt, uint32_t w, uint32_t h, int is_tex)
{
    for (uint32_t i = 0; i < nuser; i++) if (usermem[i].id == id || !usermem[i].id) { usermem[i] = (usermem_t){id, mem, fmt, w, h, (uint32_t)is_tex}; return; }
    if (nuser < MAXUSER) usermem[nuser++] = (usermem_t){id, mem, fmt, w, h, (uint32_t)is_tex};
}
static void usermem_drop(uint32_t id)
{
    for (uint32_t i = 0; i < nuser; i++) if (usermem[i].id == id) { usermem[i].id = 0; break; }
    while (nuser && !usermem[nuser - 1].id) nuser--;
}
static usermem_t *usermem_find(uint32_t id)
{
    if (!id) return NULL;
    for (uint32_t i = 0; i < nuser; i++) if (usermem[i].id == id) return &usermem[i];
    return NULL;
}
static void emit_rect(uint32_t op, const uint32_t *pre, uint32_t npre, struct lock_s *l);
/* record the current contents of user memory behind object `id` (a texture, or a plain surface) */
static void usermem_record(uint32_t id)
{
    usermem_t *u = usermem_find(id);
    if (!u) return;
    uint32_t bdim, bb = t9_block_bytes(u->fmt, &bdim);
    if (!bb) return;
    lock_t l;
    memset(&l, 0, sizeof l);
    l.id = id; l.fmt = u->fmt; l.ptr = u->mem;
    l.pitch = bdim == 4 ? (int32_t)((u->w + 3) / 4 * bb) : (int32_t)((u->w * bb + 3) & ~3u);
    l.r.right = (LONG)u->w; l.r.bottom = (LONG)u->h;
    if (u->is_tex) { const uint32_t pre[3] = {id, 0, 0}; emit_rect(T9_TEXTURE_WRITE, pre, 3, &l); }
    else emit_rect(T9_SURFACE_WRITE, NULL, 0, &l);
}

/* ---------------------------------------------------------------- resource hooks */
static ULONG WINAPI h_addref(IUnknown *o)
{
    int r = enter_resource();
    void *fn = orig(o, 1);
    ULONG c = ((ULONG (WINAPI *)(IUnknown *))fn)(o);
    if (r > 0) { uint32_t id = id_of(o); if (id) REC(T9_ADDREF, id); }
    leave_r(r);
    return c;
}
static ULONG WINAPI h_release(IUnknown *o)
{
    int r = enter_resource();
    void *fn = orig(o, 2);
    obj_t *ob = find(o);
    uint32_t id = ob ? ob->id : 0, kind = ob ? ob->kind : 0;
    ULONG c = ((ULONG (WINAPI *)(IUnknown *))fn)(o);
    if (r > 0 && id) REC(T9_RELEASE, id, (uint32_t)c);
    if (r > 0 && id && c == 0 && kind != K_SURF) forget(o);
    if (r > 0 && id && c == 0) usermem_drop(id);
    leave_r(r);
    return c;
}

static HRESULT WINAPI h_vb_lock(IDirect3DVertexBuffer9 *b, UINT off, UINT size, void **p, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DVertexBuffer9 *, UINT, UINT, void **, DWORD), b, IDirect3DVertexBuffer9, Lock)(b, off, size, p, f);
    if (r > 0 && SUCCEEDED(hr)) {
        if (f & D3DLOCK_READONLY) REC(T9_LOCK_READ, id_of(b), 0);
        else {
            lock_t *l = lock_slot(b, 0, 1);
            if (!l) note("too many outstanding locks");
            else {
                if (!size) { D3DVERTEXBUFFER_DESC d; IDirect3DVertexBuffer9_GetDesc(b, &d); size = d.Size - off; }
                l->id = id_of(b); l->offset = off; l->size = size; l->flags = f; l->ptr = *p;
            }
        }
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_ib_lock(IDirect3DIndexBuffer9 *b, UINT off, UINT size, void **p, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DIndexBuffer9 *, UINT, UINT, void **, DWORD), b, IDirect3DIndexBuffer9, Lock)(b, off, size, p, f);
    if (r > 0 && SUCCEEDED(hr)) {
        if (f & D3DLOCK_READONLY) REC(T9_LOCK_READ, id_of(b), 0);
        else {
            lock_t *l = lock_slot(b, 0, 1);
            if (!l) note("too many outstanding locks");
            else {
                if (!size) { D3DINDEXBUFFER_DESC d; IDirect3DIndexBuffer9_GetDesc(b, &d); size = d.Size - off; }
                l->id = id_of(b); l->offset = off; l->size = size; l->flags = f; l->ptr = *p;
            }
        }
    }
    leave_r(r);
    return hr;
}
static void buffer_unlock_rec(void *b)
{
    lock_t *l = lock_slot(b, 0, 0);
    if (!l) return;
    const uint32_t hdr[4] = {l->id, l->offset, l->size, l->flags};
    rec3(T9_BUFFER_WRITE, hdr, sizeof hdr, l->ptr, l->size, NULL, 0);
    l->obj = NULL;
}
static HRESULT WINAPI h_vb_unlock(IDirect3DVertexBuffer9 *b)
{
    int r = enter_resource();
    if (r > 0) buffer_unlock_rec(b);
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DVertexBuffer9 *), b, IDirect3DVertexBuffer9, Unlock)(b);
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_ib_unlock(IDirect3DIndexBuffer9 *b)
{
    int r = enter_resource();
    if (r > 0) buffer_unlock_rec(b);
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DIndexBuffer9 *), b, IDirect3DIndexBuffer9, Unlock)(b);
    leave_r(r);
    return hr;
}

static void rect_lock_info(lock_t *l, const RECT *rc, UINT w, UINT h, D3DFORMAT fmt, D3DLOCKED_RECT *lr, DWORD f, uint32_t id)
{
    l->id = id; l->flags = f; l->fmt = fmt; l->ptr = lr->pBits; l->pitch = lr->Pitch;
    l->has_rect = rc != NULL;
    if (rc) l->r = *rc; else { l->r.left = 0; l->r.top = 0; l->r.right = (LONG)w; l->r.bottom = (LONG)h; }
}

static HRESULT WINAPI h_surf_lock(IDirect3DSurface9 *s, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DSurface9 *, D3DLOCKED_RECT *, const RECT *, DWORD), s, IDirect3DSurface9, LockRect)(s, lr, rc, f);
    if (r > 0 && SUCCEEDED(hr)) {
        if (f & D3DLOCK_READONLY) REC(T9_LOCK_READ, id_of(s), 1);
        else {
            lock_t *l = lock_slot(s, 0, 1);
            D3DSURFACE_DESC d;
            IDirect3DSurface9_GetDesc(s, &d);
            if (l) rect_lock_info(l, rc, d.Width, d.Height, d.Format, lr, f, id_of(s));
        }
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_surf_unlock(IDirect3DSurface9 *s)
{
    int r = enter_resource();
    if (r > 0) {
        lock_t *l = lock_slot(s, 0, 0);
        if (l) { emit_rect(T9_SURFACE_WRITE, NULL, 0, l); l->obj = NULL; }
    }
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DSurface9 *), s, IDirect3DSurface9, UnlockRect)(s);
    leave_r(r);
    return hr;
}

static HRESULT WINAPI h_tex_lock(IDirect3DTexture9 *t, UINT lv, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DTexture9 *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD), t, IDirect3DTexture9, LockRect)(t, lv, lr, rc, f);
    if (r > 0 && SUCCEEDED(hr)) {
        if (f & D3DLOCK_READONLY) REC(T9_LOCK_READ, id_of(t), 1);
        else {
            lock_t *l = lock_slot(t, lv, 1);
            D3DSURFACE_DESC d;
            IDirect3DTexture9_GetLevelDesc(t, lv, &d);
            if (l) rect_lock_info(l, rc, d.Width, d.Height, d.Format, lr, f, id_of(t));
        }
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_tex_unlock(IDirect3DTexture9 *t, UINT lv)
{
    int r = enter_resource();
    if (r > 0) {
        lock_t *l = lock_slot(t, lv, 0);
        if (l) { const uint32_t pre[3] = {l->id, 0, lv}; emit_rect(T9_TEXTURE_WRITE, pre, 3, l); l->obj = NULL; }
    }
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DTexture9 *, UINT), t, IDirect3DTexture9, UnlockRect)(t, lv);
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_cube_lock(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT lv, D3DLOCKED_RECT *lr, const RECT *rc, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD), t, IDirect3DCubeTexture9, LockRect)(t, face, lv, lr, rc, f);
    if (r > 0 && SUCCEEDED(hr)) {
        if (f & D3DLOCK_READONLY) REC(T9_LOCK_READ, id_of(t), 1);
        else {
            lock_t *l = lock_slot(t, (uint32_t)face << 16 | lv, 1);
            D3DSURFACE_DESC d;
            IDirect3DCubeTexture9_GetLevelDesc(t, lv, &d);
            if (l) rect_lock_info(l, rc, d.Width, d.Height, d.Format, lr, f, id_of(t));
        }
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_cube_unlock(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT lv)
{
    int r = enter_resource();
    if (r > 0) {
        lock_t *l = lock_slot(t, (uint32_t)face << 16 | lv, 0);
        if (l) { const uint32_t pre[3] = {l->id, (uint32_t)face, lv}; emit_rect(T9_TEXTURE_WRITE, pre, 3, l); l->obj = NULL; }
    }
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT), t, IDirect3DCubeTexture9, UnlockRect)(t, face, lv);
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_vol_lock(IDirect3DVolumeTexture9 *t, UINT lv, D3DLOCKED_BOX *lb, const D3DBOX *bx, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DVolumeTexture9 *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD), t, IDirect3DVolumeTexture9, LockBox)(t, lv, lb, bx, f);
    if (r > 0 && SUCCEEDED(hr)) {
        if (f & D3DLOCK_READONLY) REC(T9_LOCK_READ, id_of(t), 1);
        else {
            lock_t *l = lock_slot(t, lv, 1);
            D3DVOLUME_DESC d;
            IDirect3DVolumeTexture9_GetLevelDesc(t, lv, &d);
            if (l) {
                l->id = id_of(t); l->flags = f; l->fmt = d.Format; l->ptr = lb->pBits; l->pitch = lb->RowPitch; l->slice = lb->SlicePitch;
                l->has_rect = bx != NULL;
                if (bx) l->box = *bx;
                else { l->box.Left = 0; l->box.Top = 0; l->box.Right = d.Width; l->box.Bottom = d.Height; l->box.Front = 0; l->box.Back = d.Depth; }
            }
        }
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_vol_unlock(IDirect3DVolumeTexture9 *t, UINT lv)
{
    int r = enter_resource();
    lock_t *l = r > 0 ? lock_slot(t, lv, 0) : NULL;
    if (l) {
        uint32_t bdim, bb = t9_block_bytes(l->fmt, &bdim);
        t9_box_write bw = {l->id, lv, l->flags, l->has_rect, l->box.Left, l->box.Top, l->box.Right, l->box.Bottom,
                           l->box.Front, l->box.Back, l->fmt, 0, 0, 0};
        if (bb) {
            bw.row_bytes = (l->box.Right - l->box.Left + bdim - 1) / bdim * bb;
            bw.rows = (l->box.Bottom - l->box.Top + bdim - 1) / bdim;
            bw.slices = l->box.Back - l->box.Front;
        } else note("volume lock of unknown format %u", l->fmt);
        uint32_t h[2] = {T9_VOLUME_WRITE, (uint32_t)sizeof bw + bw.row_bytes * bw.rows * bw.slices};
        put(h, 8);
        put(&bw, sizeof bw);
        for (uint32_t z = 0; z < bw.slices; z++)
            for (uint32_t y = 0; y < bw.rows; y++) put(l->ptr + (size_t)z * (size_t)l->slice + (size_t)y * (size_t)l->pitch, bw.row_bytes);
        bytes_total += 8 + h[1]; records_total++;
        l->obj = NULL;
    }
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DVolumeTexture9 *, UINT), t, IDirect3DVolumeTexture9, UnlockBox)(t, lv);
    leave_r(r);
    return hr;
}

static void patch_common(void *o, void *lockfn, int lockslot, void *unlockfn, int unlockslot)
{
    patch(o, 1, (void *)h_addref);
    patch(o, 2, (void *)h_release);
    if (lockfn) patch(o, lockslot, lockfn);
    if (unlockfn) patch(o, unlockslot, unlockfn);
}
static void patch_surface(IDirect3DSurface9 *s)
{
    if (s) patch_common(s, (void *)h_surf_lock, M_IDirect3DSurface9_LockRect, (void *)h_surf_unlock, M_IDirect3DSurface9_UnlockRect);
}

static HRESULT WINAPI h_tex_getsurf(IDirect3DTexture9 *t, UINT lv, IDirect3DSurface9 **s)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DTexture9 *, UINT, IDirect3DSurface9 **), t, IDirect3DTexture9, GetSurfaceLevel)(t, lv, s);
    if (r > 0 && SUCCEEDED(hr) && *s) {
        int n;
        uint32_t pid = id_of(t), id = got(*s, K_SURF, pid, lv, &n);
        patch_surface(*s);
        REC(T9_GET_SURFACE_LEVEL, id, pid, lv);
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_cube_getsurf(IDirect3DCubeTexture9 *t, D3DCUBEMAP_FACES face, UINT lv, IDirect3DSurface9 **s)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DCubeTexture9 *, D3DCUBEMAP_FACES, UINT, IDirect3DSurface9 **), t, IDirect3DCubeTexture9, GetCubeMapSurface)(t, face, lv, s);
    if (r > 0 && SUCCEEDED(hr) && *s) {
        int n;
        uint32_t pid = id_of(t), id = got(*s, K_SURF, pid, (uint32_t)face << 16 | lv, &n);
        patch_surface(*s);
        REC(T9_GET_CUBE_SURFACE, id, pid, (uint32_t)face, lv);
    }
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_tex_genmips(IDirect3DTexture9 *t)
{
    int r = enter_resource();
    if (r > 0) REC(T9_GEN_MIPS, id_of(t));
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DTexture9 *), t, IDirect3DTexture9, GenerateMipSubLevels)(t);
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_q_issue(IDirect3DQuery9 *q, DWORD f)
{
    int r = enter_resource();
    if (r > 0) REC(T9_QUERY_ISSUE, id_of(q), f);
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DQuery9 *, DWORD), q, IDirect3DQuery9, Issue)(q, f);
    leave_r(r);
    return hr;
}
static HRESULT WINAPI h_q_getdata(IDirect3DQuery9 *q, void *data, DWORD size, DWORD f)
{
    int r = enter_resource();
    HRESULT hr = ORIG(HRESULT (WINAPI *)(IDirect3DQuery9 *, void *, DWORD, DWORD), q, IDirect3DQuery9, GetData)(q, data, size, f);
    if (r > 0) REC(T9_QUERY_GETDATA, id_of(q), size, f, (uint32_t)hr);
    leave_r(r);
    return hr;
}

/* ---------------------------------------------------------------- device hooks (chain to the previous slot) */
static void *prev_dev[DEV_NMETHODS], *prev_d3d[D3D_NMETHODS];
static uint8_t hooked[DEV_NMETHODS];
#define PREV(m, T) ((T)prev_dev[DEV_##m])
typedef IDirect3DDevice9Ex DEV;

static t9_pp to_pp(const D3DPRESENT_PARAMETERS *p)
{
    t9_pp t = {p->BackBufferWidth, p->BackBufferHeight, (uint32_t)p->BackBufferFormat, p->BackBufferCount,
               (uint32_t)p->MultiSampleType, p->MultiSampleQuality, (uint32_t)p->SwapEffect, (uint32_t)p->Windowed,
               (uint32_t)p->EnableAutoDepthStencil, (uint32_t)p->AutoDepthStencilFormat, p->Flags,
               p->FullScreen_RefreshRateInHz, p->PresentationInterval};
    return t;
}

/* simple state setters: record then forward */
#define SIMPLE2(m, op, T1, T2) \
    static HRESULT WINAPI k_##m(DEV *self, T1 a, T2 b) { int r = enter(); if (r) REC(op, (uint32_t)a, (uint32_t)b); \
        HRESULT hr = PREV(m, HRESULT (WINAPI *)(DEV *, T1, T2))(self, a, b); leave(); return hr; }
#define SIMPLE3(m, op, T1, T2, T3) \
    static HRESULT WINAPI k_##m(DEV *self, T1 a, T2 b, T3 c) { int r = enter(); if (r) REC(op, (uint32_t)a, (uint32_t)b, (uint32_t)c); \
        HRESULT hr = PREV(m, HRESULT (WINAPI *)(DEV *, T1, T2, T3))(self, a, b, c); leave(); return hr; }
SIMPLE2(SetRenderState, T9_SET_RENDER_STATE, D3DRENDERSTATETYPE, DWORD)
SIMPLE3(SetSamplerState, T9_SET_SAMPLER_STATE, DWORD, D3DSAMPLERSTATETYPE, DWORD)
SIMPLE3(SetTextureStageState, T9_SET_TSS, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD)
SIMPLE2(SetStreamSourceFreq, T9_SET_STREAM_FREQ, UINT, UINT)
SIMPLE3(DrawPrimitive, T9_DRAW, D3DPRIMITIVETYPE, UINT, UINT)

#define OBJ1(m, op, T) \
    static HRESULT WINAPI k_##m(DEV *self, T o) { int r = enter(); if (r) REC(op, id_of(o)); \
        HRESULT hr = PREV(m, HRESULT (WINAPI *)(DEV *, T))(self, o); leave(); return hr; }
OBJ1(SetIndices, T9_SET_INDICES, IDirect3DIndexBuffer9 *)
OBJ1(SetVertexDeclaration, T9_SET_VDECL, IDirect3DVertexDeclaration9 *)
OBJ1(SetVertexShader, T9_SET_VS, IDirect3DVertexShader9 *)
OBJ1(SetPixelShader, T9_SET_PS, IDirect3DPixelShader9 *)
OBJ1(SetDepthStencilSurface, T9_SET_DS, IDirect3DSurface9 *)

static HRESULT WINAPI k_SetFVF(DEV *self, DWORD fvf)
{ int r = enter(); if (r) REC(T9_SET_FVF, fvf); HRESULT hr = PREV(SetFVF, HRESULT (WINAPI *)(DEV *, DWORD))(self, fvf); leave(); return hr; }

static HRESULT WINAPI k_SetTexture(DEV *self, DWORD st, IDirect3DBaseTexture9 *t)
{ int r = enter(); if (r) REC(T9_SET_TEXTURE, st, id_of(t)); HRESULT hr = PREV(SetTexture, HRESULT (WINAPI *)(DEV *, DWORD, IDirect3DBaseTexture9 *))(self, st, t); leave(); return hr; }
static HRESULT WINAPI k_SetRenderTarget(DEV *self, DWORD i, IDirect3DSurface9 *s)
{ int r = enter(); if (r) REC(T9_SET_RT, i, id_of(s)); HRESULT hr = PREV(SetRenderTarget, HRESULT (WINAPI *)(DEV *, DWORD, IDirect3DSurface9 *))(self, i, s); leave(); return hr; }
static HRESULT WINAPI k_SetStreamSource(DEV *self, UINT s, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{ int r = enter(); if (r) REC(T9_SET_STREAM_SOURCE, s, id_of(vb), off, stride);
  HRESULT hr = PREV(SetStreamSource, HRESULT (WINAPI *)(DEV *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT))(self, s, vb, off, stride); leave(); return hr; }
static HRESULT WINAPI k_DrawIndexedPrimitive(DEV *self, D3DPRIMITIVETYPE t, INT base, UINT mi, UINT nv, UINT start, UINT pc)
{ int r = enter(); if (r) REC(T9_DRAW_INDEXED, t, (uint32_t)base, mi, nv, start, pc);
  HRESULT hr = PREV(DrawIndexedPrimitive, HRESULT (WINAPI *)(DEV *, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT))(self, t, base, mi, nv, start, pc); leave(); return hr; }

static UINT prim_verts(D3DPRIMITIVETYPE t, UINT pc)
{
    switch (t) {
    case D3DPT_POINTLIST: return pc;
    case D3DPT_LINELIST: return pc * 2;
    case D3DPT_LINESTRIP: return pc + 1;
    case D3DPT_TRIANGLELIST: return pc * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return pc + 2;
    default: return 0;
    }
}
static HRESULT WINAPI k_DrawPrimitiveUP(DEV *self, D3DPRIMITIVETYPE t, UINT pc, const void *d, UINT stride)
{
    int r = enter();
    if (r) { uint32_t n = prim_verts(t, pc) * stride; const uint32_t h[4] = {t, pc, stride, n}; rec3(T9_DRAW_UP, h, sizeof h, d, n, NULL, 0); }
    HRESULT hr = PREV(DrawPrimitiveUP, HRESULT (WINAPI *)(DEV *, D3DPRIMITIVETYPE, UINT, const void *, UINT))(self, t, pc, d, stride);
    leave(); return hr;
}
static HRESULT WINAPI k_DrawIndexedPrimitiveUP(DEV *self, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT pc, const void *idx, D3DFORMAT ifmt,
                                               const void *v, UINT stride)
{
    int r = enter();
    if (r) {
        uint32_t ib = prim_verts(t, pc) * (ifmt == D3DFMT_INDEX32 ? 4 : 2), vb = (mi + nv) * stride;
        const uint32_t h[8] = {t, mi, nv, pc, ifmt, stride, ib, vb};
        rec3(T9_DRAW_INDEXED_UP, h, sizeof h, idx, ib, v, vb);
    }
    HRESULT hr = PREV(DrawIndexedPrimitiveUP, HRESULT (WINAPI *)(DEV *, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void *, D3DFORMAT, const void *, UINT))(self, t, mi, nv, pc, idx, ifmt, v, stride);
    leave(); return hr;
}

#define CONSTS(m, op, T, per) \
    static HRESULT WINAPI k_##m(DEV *self, UINT start, const T *d, UINT n) { int r = enter(); \
        if (r) { const uint32_t h[2] = {start, n}; rec3(op, h, 8, d, n * (per) * 4, NULL, 0); } \
        HRESULT hr = PREV(m, HRESULT (WINAPI *)(DEV *, UINT, const T *, UINT))(self, start, d, n); leave(); return hr; }
CONSTS(SetVertexShaderConstantF, T9_VS_CONST_F, float, 4)
CONSTS(SetVertexShaderConstantI, T9_VS_CONST_I, int, 4)
CONSTS(SetVertexShaderConstantB, T9_VS_CONST_B, BOOL, 1)
CONSTS(SetPixelShaderConstantF, T9_PS_CONST_F, float, 4)
CONSTS(SetPixelShaderConstantI, T9_PS_CONST_I, int, 4)
CONSTS(SetPixelShaderConstantB, T9_PS_CONST_B, BOOL, 1)

static HRESULT WINAPI k_SetViewport(DEV *self, const D3DVIEWPORT9 *v)
{ int r = enter(); if (r) rec3(T9_SET_VIEWPORT, v, sizeof *v, NULL, 0, NULL, 0); HRESULT hr = PREV(SetViewport, HRESULT (WINAPI *)(DEV *, const D3DVIEWPORT9 *))(self, v); leave(); return hr; }
static HRESULT WINAPI k_SetScissorRect(DEV *self, const RECT *rc)
{ int r = enter(); if (r) rec3(T9_SET_SCISSOR, rc, sizeof *rc, NULL, 0, NULL, 0); HRESULT hr = PREV(SetScissorRect, HRESULT (WINAPI *)(DEV *, const RECT *))(self, rc); leave(); return hr; }
static HRESULT WINAPI k_SetClipPlane(DEV *self, DWORD i, const float *p)
{ int r = enter(); if (r) rec3(T9_SET_CLIP_PLANE, &i, 4, p, 16, NULL, 0); HRESULT hr = PREV(SetClipPlane, HRESULT (WINAPI *)(DEV *, DWORD, const float *))(self, i, p); leave(); return hr; }
static HRESULT WINAPI k_SetTransform(DEV *self, D3DTRANSFORMSTATETYPE s, const D3DMATRIX *m)
{ int r = enter(); if (r) rec3(T9_SET_TRANSFORM, &s, 4, m, 64, NULL, 0); HRESULT hr = PREV(SetTransform, HRESULT (WINAPI *)(DEV *, D3DTRANSFORMSTATETYPE, const D3DMATRIX *))(self, s, m); leave(); return hr; }
static HRESULT WINAPI k_SetMaterial(DEV *self, const D3DMATERIAL9 *m)
{ int r = enter(); if (r) rec3(T9_SET_MATERIAL, m, sizeof *m, NULL, 0, NULL, 0); HRESULT hr = PREV(SetMaterial, HRESULT (WINAPI *)(DEV *, const D3DMATERIAL9 *))(self, m); leave(); return hr; }
static void WINAPI k_SetGammaRamp(DEV *self, UINT sc, DWORD f, const D3DGAMMARAMP *g)
{ int r = enter(); if (r) { const uint32_t h[2] = {sc, f}; rec3(T9_SET_GAMMA_RAMP, h, 8, g, sizeof *g, NULL, 0); }
  PREV(SetGammaRamp, void (WINAPI *)(DEV *, UINT, DWORD, const D3DGAMMARAMP *))(self, sc, f, g); leave(); }

static HRESULT WINAPI k_Clear(DEV *self, DWORD n, const D3DRECT *rc, DWORD f, D3DCOLOR c, float z, DWORD st)
{
    int r = enter();
    if (r) { t9_clear h = {rc ? n : 0, f, c, z, st}; rec3(T9_CLEAR, &h, sizeof h, rc, rc ? n * 16 : 0, NULL, 0); }
    HRESULT hr = PREV(Clear, HRESULT (WINAPI *)(DEV *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD))(self, n, rc, f, c, z, st);
    leave(); return hr;
}
static HRESULT WINAPI k_BeginScene(DEV *self)
{ int r = enter(); if (r) rec3(T9_BEGIN_SCENE, NULL, 0, NULL, 0, NULL, 0); HRESULT hr = PREV(BeginScene, HRESULT (WINAPI *)(DEV *))(self); leave(); return hr; }
static HRESULT WINAPI k_EndScene(DEV *self)
{ int r = enter(); if (r) rec3(T9_END_SCENE, NULL, 0, NULL, 0, NULL, 0); HRESULT hr = PREV(EndScene, HRESULT (WINAPI *)(DEV *))(self); leave(); return hr; }
static HRESULT WINAPI k_EvictManagedResources(DEV *self)
{ int r = enter(); if (r) rec3(T9_EVICT_MANAGED, NULL, 0, NULL, 0, NULL, 0); HRESULT hr = PREV(EvictManagedResources, HRESULT (WINAPI *)(DEV *))(self); leave(); return hr; }

static void frame_done(void)
{
    if (++frames % 120 == 0) flush_out();   /* a killed process still leaves a usable prefix */
}
static HRESULT WINAPI k_Present(DEV *self, const RECT *s, const RECT *d, HWND w, const RGNDATA *rg)
{
    int r = enter();
    if (r) rec3(T9_PRESENT, NULL, 0, NULL, 0, NULL, 0);
    HRESULT hr = PREV(Present, HRESULT (WINAPI *)(DEV *, const RECT *, const RECT *, HWND, const RGNDATA *))(self, s, d, w, rg);
    if (r) frame_done();
    leave(); return hr;
}
static HRESULT WINAPI k_PresentEx(DEV *self, const RECT *s, const RECT *d, HWND w, const RGNDATA *rg, DWORD f)
{
    int r = enter();
    if (r) rec3(T9_PRESENT, NULL, 0, NULL, 0, NULL, 0);
    HRESULT hr = PREV(PresentEx, HRESULT (WINAPI *)(DEV *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD))(self, s, d, w, rg, f);
    if (r) frame_done();
    leave(); return hr;
}
static HRESULT WINAPI k_Reset(DEV *self, D3DPRESENT_PARAMETERS *pp)
{
    int r = enter();
    if (r) { t9_pp p = to_pp(pp); rec3(T9_RESET, &p, sizeof p, NULL, 0, NULL, 0); }
    HRESULT hr = PREV(Reset, HRESULT (WINAPI *)(DEV *, D3DPRESENT_PARAMETERS *))(self, pp);
    leave(); return hr;
}

static void opt_rect(uint32_t *dst, const RECT *rc) { dst[0] = rc != NULL; if (rc) memcpy(dst + 1, rc, 16); else memset(dst + 1, 0, 16); }
static HRESULT WINAPI k_StretchRect(DEV *self, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *d, const RECT *dr, D3DTEXTUREFILTERTYPE f)
{
    int r = enter();
    if (r) { uint32_t p[13]; p[0] = id_of(s); opt_rect(p + 1, sr); p[6] = id_of(d); opt_rect(p + 7, dr); p[12] = f; rec3(T9_STRETCH_RECT, p, sizeof p, NULL, 0, NULL, 0); }
    HRESULT hr = PREV(StretchRect, HRESULT (WINAPI *)(DEV *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const RECT *, D3DTEXTUREFILTERTYPE))(self, s, sr, d, dr, f);
    leave(); return hr;
}
static HRESULT WINAPI k_UpdateSurface(DEV *self, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *d, const POINT *pt)
{
    int r = enter();
    if (r) {
        obj_t *so = find(s);
        if (so) usermem_record(so->parent && so->sub == 0 && usermem_find(so->parent) ? so->parent : so->id);
        uint32_t p[10]; p[0] = id_of(s); opt_rect(p + 1, sr); p[6] = id_of(d); p[7] = pt != NULL;
        p[8] = pt ? (uint32_t)pt->x : 0; p[9] = pt ? (uint32_t)pt->y : 0;
        rec3(T9_UPDATE_SURFACE, p, sizeof p, NULL, 0, NULL, 0);
    }
    HRESULT hr = PREV(UpdateSurface, HRESULT (WINAPI *)(DEV *, IDirect3DSurface9 *, const RECT *, IDirect3DSurface9 *, const POINT *))(self, s, sr, d, pt);
    leave(); return hr;
}
static HRESULT WINAPI k_UpdateTexture(DEV *self, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *d)
{ int r = enter(); if (r) { usermem_record(id_of(s)); REC(T9_UPDATE_TEXTURE, id_of(s), id_of(d)); }
  HRESULT hr = PREV(UpdateTexture, HRESULT (WINAPI *)(DEV *, IDirect3DBaseTexture9 *, IDirect3DBaseTexture9 *))(self, s, d); leave(); return hr; }
static HRESULT WINAPI k_GetRenderTargetData(DEV *self, IDirect3DSurface9 *s, IDirect3DSurface9 *d)
{ int r = enter(); if (r) REC(T9_GET_RT_DATA, id_of(s), id_of(d));
  HRESULT hr = PREV(GetRenderTargetData, HRESULT (WINAPI *)(DEV *, IDirect3DSurface9 *, IDirect3DSurface9 *))(self, s, d); leave(); return hr; }
static HRESULT WINAPI k_ColorFill(DEV *self, IDirect3DSurface9 *s, const RECT *rc, D3DCOLOR c)
{
    int r = enter();
    if (r) { uint32_t p[7]; p[0] = id_of(s); opt_rect(p + 1, rc); p[6] = c; rec3(T9_COLOR_FILL, p, sizeof p, NULL, 0, NULL, 0); }
    HRESULT hr = PREV(ColorFill, HRESULT (WINAPI *)(DEV *, IDirect3DSurface9 *, const RECT *, D3DCOLOR))(self, s, rc, c);
    leave(); return hr;
}

/* getters returning surfaces */
static HRESULT WINAPI k_GetRenderTarget(DEV *self, DWORD i, IDirect3DSurface9 **s)
{
    int r = enter();
    HRESULT hr = PREV(GetRenderTarget, HRESULT (WINAPI *)(DEV *, DWORD, IDirect3DSurface9 **))(self, i, s);
    if (r && SUCCEEDED(hr) && *s) {
        int n; obj_t *o = find(*s);
        uint32_t id = o ? o->id : got(*s, K_SURF, 0, 0x7f000000u | i, &n);   /* known surface: same id */
        patch_surface(*s);
        REC(T9_GET_RT, id, i);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_GetDepthStencilSurface(DEV *self, IDirect3DSurface9 **s)
{
    int r = enter();
    HRESULT hr = PREV(GetDepthStencilSurface, HRESULT (WINAPI *)(DEV *, IDirect3DSurface9 **))(self, s);
    if (r && SUCCEEDED(hr) && *s) {
        int n; obj_t *o = find(*s);
        uint32_t id = o ? o->id : got(*s, K_SURF, 0, 0x7e000000u, &n);
        patch_surface(*s);
        REC(T9_GET_DS, id);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_GetBackBuffer(DEV *self, UINT sc, UINT i, D3DBACKBUFFER_TYPE t, IDirect3DSurface9 **s)
{
    int r = enter();
    HRESULT hr = PREV(GetBackBuffer, HRESULT (WINAPI *)(DEV *, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **))(self, sc, i, t, s);
    if (r && SUCCEEDED(hr) && *s) {
        int n; obj_t *o = find(*s);
        uint32_t id = o ? o->id : got(*s, K_SURF, 0, 0x7d000000u | sc << 8 | i, &n);
        patch_surface(*s);
        REC(T9_GET_BACKBUFFER, id, sc, i, t);
    }
    leave(); return hr;
}

/* creation */
static HRESULT WINAPI k_CreateTexture(DEV *self, UINT w, UINT h, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateTexture, HRESULT (WINAPI *)(DEV *, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9 **, HANDLE *))(self, w, h, lv, u, f, p, o, sh);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, (void *)h_tex_lock, M_IDirect3DTexture9_LockRect, (void *)h_tex_unlock, M_IDirect3DTexture9_UnlockRect);
        patch(*o, M_IDirect3DTexture9_GetSurfaceLevel, (void *)h_tex_getsurf);
        patch(*o, M_IDirect3DTexture9_GenerateMipSubLevels, (void *)h_tex_genmips);
        uint32_t id = add(*o, K_TEX, 0, 0);
        REC(T9_CREATE_TEXTURE, id, w, h, lv, u, f, p);
        if (p == D3DPOOL_SYSTEMMEM && sh && *sh) usermem_add(id, *sh, f, w, h, 1);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateCubeTexture(DEV *self, UINT e, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateCubeTexture, HRESULT (WINAPI *)(DEV *, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9 **, HANDLE *))(self, e, lv, u, f, p, o, sh);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, (void *)h_cube_lock, M_IDirect3DCubeTexture9_LockRect, (void *)h_cube_unlock, M_IDirect3DCubeTexture9_UnlockRect);
        patch(*o, M_IDirect3DCubeTexture9_GetCubeMapSurface, (void *)h_cube_getsurf);
        REC(T9_CREATE_CUBE, add(*o, K_CUBE, 0, 0), e, lv, u, f, p);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateVolumeTexture(DEV *self, UINT w, UINT h, UINT d, UINT lv, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateVolumeTexture, HRESULT (WINAPI *)(DEV *, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9 **, HANDLE *))(self, w, h, d, lv, u, f, p, o, sh);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, (void *)h_vol_lock, M_IDirect3DVolumeTexture9_LockBox, (void *)h_vol_unlock, M_IDirect3DVolumeTexture9_UnlockBox);
        REC(T9_CREATE_VOLUME, add(*o, K_VOL, 0, 0), w, h, d, lv, u, f, p);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateVertexBuffer(DEV *self, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateVertexBuffer, HRESULT (WINAPI *)(DEV *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *))(self, len, u, fvf, p, o, sh);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, (void *)h_vb_lock, M_IDirect3DVertexBuffer9_Lock, (void *)h_vb_unlock, M_IDirect3DVertexBuffer9_Unlock);
        REC(T9_CREATE_VB, add(*o, K_VB, 0, 0), len, u, fvf, p);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateIndexBuffer(DEV *self, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateIndexBuffer, HRESULT (WINAPI *)(DEV *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *))(self, len, u, f, p, o, sh);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, (void *)h_ib_lock, M_IDirect3DIndexBuffer9_Lock, (void *)h_ib_unlock, M_IDirect3DIndexBuffer9_Unlock);
        REC(T9_CREATE_IB, add(*o, K_IB, 0, 0), len, u, f, p);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateRenderTarget(DEV *self, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL lk, IDirect3DSurface9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateRenderTarget, HRESULT (WINAPI *)(DEV *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *))(self, w, h, f, ms, q, lk, o, sh);
    if (r && SUCCEEDED(hr) && *o) { patch_surface(*o); REC(T9_CREATE_RT, add(*o, K_SURF, 0, 0), w, h, f, ms, q, lk); }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateDepthStencilSurface(DEV *self, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL dc, IDirect3DSurface9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateDepthStencilSurface, HRESULT (WINAPI *)(DEV *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **, HANDLE *))(self, w, h, f, ms, q, dc, o, sh);
    if (r && SUCCEEDED(hr) && *o) { patch_surface(*o); REC(T9_CREATE_DS, add(*o, K_SURF, 0, 0), w, h, f, ms, q, dc); }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateOffscreenPlainSurface(DEV *self, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9 **o, HANDLE *sh)
{
    int r = enter();
    HRESULT hr = PREV(CreateOffscreenPlainSurface, HRESULT (WINAPI *)(DEV *, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9 **, HANDLE *))(self, w, h, f, p, o, sh);
    if (r && SUCCEEDED(hr) && *o) {
        patch_surface(*o);
        uint32_t id = add(*o, K_SURF, 0, 0);
        REC(T9_CREATE_OFFSCREEN, id, w, h, f, p);
        if (p == D3DPOOL_SYSTEMMEM && sh && *sh) usermem_add(id, *sh, f, w, h, 0);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateQuery(DEV *self, D3DQUERYTYPE t, IDirect3DQuery9 **o)
{
    int r = enter();
    HRESULT hr = PREV(CreateQuery, HRESULT (WINAPI *)(DEV *, D3DQUERYTYPE, IDirect3DQuery9 **))(self, t, o);
    if (r && SUCCEEDED(hr) && o && *o) {
        patch_common(*o, (void *)h_q_issue, M_IDirect3DQuery9_Issue, (void *)h_q_getdata, M_IDirect3DQuery9_GetData);
        REC(T9_CREATE_QUERY, add(*o, K_QUERY, 0, 0), t);
    }
    leave(); return hr;
}
static UINT shader_dwords(const DWORD *code)
{
    UINT n = 1;
    while (code[n] != 0x0000ffff) n += ((code[n] & 0xffff) == 0xfffe) ? 1 + (code[n] >> 16) : 1;
    return n + 1;
}
static HRESULT WINAPI k_CreateVertexShader(DEV *self, const DWORD *code, IDirect3DVertexShader9 **o)
{
    int r = enter();
    HRESULT hr = PREV(CreateVertexShader, HRESULT (WINAPI *)(DEV *, const DWORD *, IDirect3DVertexShader9 **))(self, code, o);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, NULL, 0, NULL, 0);
        uint32_t h[2] = {add(*o, K_VS, 0, 0), shader_dwords(code)};
        rec3(T9_CREATE_VS, h, 8, code, h[1] * 4, NULL, 0);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreatePixelShader(DEV *self, const DWORD *code, IDirect3DPixelShader9 **o)
{
    int r = enter();
    HRESULT hr = PREV(CreatePixelShader, HRESULT (WINAPI *)(DEV *, const DWORD *, IDirect3DPixelShader9 **))(self, code, o);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, NULL, 0, NULL, 0);
        uint32_t h[2] = {add(*o, K_PS, 0, 0), shader_dwords(code)};
        rec3(T9_CREATE_PS, h, 8, code, h[1] * 4, NULL, 0);
    }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateVertexDeclaration(DEV *self, const D3DVERTEXELEMENT9 *e, IDirect3DVertexDeclaration9 **o)
{
    int r = enter();
    HRESULT hr = PREV(CreateVertexDeclaration, HRESULT (WINAPI *)(DEV *, const D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **))(self, e, o);
    if (r && SUCCEEDED(hr) && *o) {
        patch_common(*o, NULL, 0, NULL, 0);
        uint32_t n = 1;
        while (e[n - 1].Stream != 0xff) n++;
        uint32_t h[2] = {add(*o, K_DECL, 0, 0), n};
        rec3(T9_CREATE_VDECL, h, 8, e, n * sizeof *e, NULL, 0);
    }
    leave(); return hr;
}

/* IDirect3D9: device creation */
static HRESULT WINAPI k_CreateDevice(IDirect3D9Ex *self, UINT ad, D3DDEVTYPE t, HWND fw, DWORD fl, D3DPRESENT_PARAMETERS *pp, IDirect3DDevice9 **o)
{
    int r = enter();
    HRESULT hr = ((HRESULT (WINAPI *)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **))prev_d3d[D3D_CreateDevice])(self, ad, t, fw, fl, pp, o);
    if (r && SUCCEEDED(hr)) { t9_create_device c = {ad, t, fl, 0, to_pp(pp)}; rec3(T9_CREATE_DEVICE, &c, sizeof c, NULL, 0, NULL, 0); }
    leave(); return hr;
}
static HRESULT WINAPI k_CreateDeviceEx(IDirect3D9Ex *self, UINT ad, D3DDEVTYPE t, HWND fw, DWORD fl, D3DPRESENT_PARAMETERS *pp, D3DDISPLAYMODEEX *m, IDirect3DDevice9Ex **o)
{
    int r = enter();
    HRESULT hr = ((HRESULT (WINAPI *)(IDirect3D9Ex *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, D3DDISPLAYMODEEX *, IDirect3DDevice9Ex **))prev_d3d[D3D_CreateDeviceEx])(self, ad, t, fw, fl, pp, m, o);
    if (r && SUCCEEDED(hr)) { t9_create_device c = {ad, t, fl, 1, to_pp(pp)}; rec3(T9_CREATE_DEVICE, &c, sizeof c, NULL, 0, NULL, 0); }
    leave(); return hr;
}

/* ---------------------------------------------------------------- unrecorded-method audit (via census thunks) */
void capture_hit(DWORD code)
{
    static uint8_t seen[2][256];
    uint32_t k = code >> 16, m = code & 0xffff;
    if (k > 1 || m > 255 || seen[k][m] || (k == 1 && hooked[m])) return;   /* hooks chain through the audit thunks */
    seen[k][m] = 1;
    /* stateless queries the replay does not need */
    static const char *const ignore[] = {"TestCooperativeLevel", "GetDeviceCaps", "GetDisplayMode", "GetCreationParameters",
        "GetAvailableTextureMem", "GetDirect3D", "QueryInterface", "AddRef", "GetSwapChain", "GetNumberOfSwapChains",
        "GetRasterStatus", "ShowCursor", "SetCursorPosition", "GetViewport", "GetRenderState", "GetSamplerState",
        "GetTexture", "GetStreamSource", "GetIndices", "GetVertexShader", "GetPixelShader", "GetVertexDeclaration",
        "GetTransform", "GetClipPlane", "GetScissorRect", "GetFVF", "GetGammaRamp", "GetAdapterCount",
        "GetAdapterIdentifier", "GetAdapterModeCount", "EnumAdapterModes", "GetAdapterDisplayMode", "CheckDeviceType",
        "CheckDeviceFormat", "CheckDeviceMultiSampleType", "CheckDepthStencilMatch", "CheckDeviceFormatConversion",
        "GetAdapterMonitor", "RegisterSoftwareDevice", "GetAdapterModeCountEx", "EnumAdapterModesEx",
        "GetAdapterDisplayModeEx", "GetAdapterLUID", "Release"};
    const char *name = k ? dev_method_names[m] : d3d_method_names[m];
    for (unsigned i = 0; i < sizeof ignore / sizeof *ignore; i++) if (!strcmp(name, ignore[i])) return;
    EnterCriticalSection(&cap_lock);
    note("UNRECORDED %s.%s", k ? "dev" : "d3d", name);
    LeaveCriticalSection(&cap_lock);
}

void capture_close(void)
{
    if (!capture_on) return;
    EnterCriticalSection(&cap_lock);
    flush_out();
    info("capture: %llu records, %llu bytes, %llu frames, %u ids\n", (unsigned long long)records_total,
         (unsigned long long)bytes_total, (unsigned long long)frames, next_id - 1);
    LeaveCriticalSection(&cap_lock);
}

void capture_install(void **d3d_vt, void **dev_vt, const char *dir, const char *tag)
{
    char mode[32] = "";
    GetEnvironmentVariableA("TF2MT_TRACE_MODE", mode, sizeof mode);
    if (strcmp(mode, "capture")) return;
    InitializeCriticalSection(&cap_lock);
    tls_depth = TlsAlloc();
    buf = VirtualAlloc(NULL, BUF_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    objs = VirtualAlloc(NULL, sizeof(obj_t) * OBJ_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\capture-%s.t9", dir, tag);
    out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE || !buf || !objs) { info("capture: cannot open %s\n", path); return; }
    t9_header h = {T9_MAGIC, T9_VERSION, 0, 0};
    put(&h, sizeof h);
    capture_on = 1;

    /* audit thunks on every slot first (unrecorded methods get logged once), then the recording hooks */
    for (int i = 0; i < DEV_NMETHODS; i++) if (dev_vt[i] == dev_thunks[i]) dev_vt[i] = dev_cthunks[i];
    for (int i = 0; i < D3D_NMETHODS; i++) if (d3d_vt[i] == d3d_thunks[i]) d3d_vt[i] = d3d_cthunks[i];
    memcpy(prev_dev, dev_vt, sizeof prev_dev);
    memcpy(prev_d3d, d3d_vt, sizeof prev_d3d);
#define K(m) (dev_vt[DEV_##m] = (void *)k_##m, hooked[DEV_##m] = 1)
    K(SetRenderState); K(SetSamplerState); K(SetTextureStageState); K(SetStreamSourceFreq); K(DrawPrimitive);
    K(SetIndices); K(SetVertexDeclaration); K(SetVertexShader); K(SetPixelShader); K(SetDepthStencilSurface); K(SetFVF);
    K(SetTexture); K(SetRenderTarget); K(SetStreamSource); K(DrawIndexedPrimitive); K(DrawPrimitiveUP); K(DrawIndexedPrimitiveUP);
    K(SetVertexShaderConstantF); K(SetVertexShaderConstantI); K(SetVertexShaderConstantB);
    K(SetPixelShaderConstantF); K(SetPixelShaderConstantI); K(SetPixelShaderConstantB);
    K(SetViewport); K(SetScissorRect); K(SetClipPlane); K(SetTransform); K(SetMaterial); K(SetGammaRamp);
    K(Clear); K(BeginScene); K(EndScene); K(EvictManagedResources); K(Present); K(PresentEx); K(Reset);
    K(StretchRect); K(UpdateSurface); K(UpdateTexture); K(GetRenderTargetData); K(ColorFill);
    K(GetRenderTarget); K(GetDepthStencilSurface); K(GetBackBuffer);
    K(CreateTexture); K(CreateCubeTexture); K(CreateVolumeTexture); K(CreateVertexBuffer); K(CreateIndexBuffer);
    K(CreateRenderTarget); K(CreateDepthStencilSurface); K(CreateOffscreenPlainSurface); K(CreateQuery);
    K(CreateVertexShader); K(CreatePixelShader); K(CreateVertexDeclaration);
    d3d_vt[D3D_CreateDevice] = (void *)k_CreateDevice;
    d3d_vt[D3D_CreateDeviceEx] = (void *)k_CreateDeviceEx;
    info("capture: writing %s\n", path);
}
