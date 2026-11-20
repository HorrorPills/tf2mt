// Relative mouse mode (issue #2: mouse input locked the game to the display refresh rate).
//
// Problem: macOS shows a window's CAMetalLayer frames directly (no compositor pass) while nothing else on screen
// changes. Every change of the cursor position, even of a hidden cursor, makes WindowServer compose the screen again.
// The composited path delivers at most one frame per display refresh and queues ~3 frames. TF2 under Wine moves the
// cursor all the time: the mouse moves it, and the game re-centres it every frame (SetCursorPos -> CGWarpMouse-
// CursorPosition). Measured (60 Hz display, fps_max 120, no vsync): still 120 fps / 12 ms frame start -> on screen;
// turning 60 fps / 65 ms. Mouse deltas without a cursor move: no drop (tools/bench/mouse-fps.sh, docs/mouse-input.md).
//
// Fix: while the game hides the cursor and re-centres it (in-game aim), the cursor stays parked at the game's centre:
//  * CGAssociateMouseAndMouseCursorPosition(false): mouse motion no longer moves the macOS cursor; events still carry
//    the device deltas.
//  * -[WineApplicationController handleMouseMove:]: Wine sends absolute positions while the cursor could move further
//    inside the screens (and the clip rect), and relative deltas only when it is pinned at an edge. For the call, the
//    cached screen-rect list is empty, so every motion counts as pinned -> MOUSE_MOVED_RELATIVE (device deltas, also
//    the WM_INPUT raw deltas). Wine's own code still builds and queues the event.
//  * -[WineApplicationController setCursorPosition:]: the game's per-frame re-centre does not warp the cursor. The
//    first one (or one to a new point) parks the cursor there, so GetCursorPos (which asks the driver when the cursor
//    has not moved for 100 ms) returns the centre and m_rawinput 0 sees no drift. No mouse-move events are discarded
//    (the old winemac patch is not needed in this mode).
// The mode starts at the second re-centre to the same point within 1 s while the cursor is hidden (a hidden cursor
// alone could be a game drawing its own pointer). Normal Wine behaviour returns when the cursor is shown (menus) or the
// app is inactive.
// Off with TF2MT_RELMOUSE=0. All hooks run on Wine's Cocoa main thread (the macdrv calls them there).
//
// Built twice: into tf2mt.so (Metal renderer; installed at device init) and as relmouse.dylib (-DRELMOUSE_DYLIB),
// which scripts/tf2.sh loads into the game with DYLD_INSERT_LIBRARIES so DXVK sessions get it too. The dylib installs
// once winemac.so is loaded. Whichever copy comes first installs; the other sees the marker and does nothing.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

#ifdef RELMOUSE_DYLIB
// log file: TF2MT_RELMOUSE_LOG (unix path, opened on first use)
static void tf2mt_log(const char *fmt, ...)
{
    static FILE *f;
    if (!f) { const char *p = getenv("TF2MT_RELMOUSE_LOG"); if (!p || !(f = fopen(p, "a"))) return; }
    fprintf(f, "[%d] ", getpid());
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fflush(f);
}
#else
void tf2mt_log(const char *fmt, ...);
#endif

static Class g_ctl_class;
static Ivar g_iv_wants_hidden, g_iv_force_abs, g_iv_screen_rects;
static void (*orig_handle_mouse_move)(id, SEL, NSEvent *);
static BOOL (*orig_set_cursor_position)(id, SEL, CGPoint);
static void (*orig_unhide_cursor)(id, SEL);

static BOOL g_rel;                 // relative mode active (mouse disassociated from the cursor)
static CGPoint g_park;             // where the cursor is parked in relative mode
static CGPoint g_last_set;         // re-centre detection (normal mode)
static double g_last_set_t;
static int g_same_sets;
static unsigned long long g_rel_events, g_warps_skipped, g_entries;

static BOOL ivar_bool(id obj, Ivar iv) { return *(BOOL *)((char *)(__bridge void *)obj + ivar_getOffset(iv)); }
static void set_ivar_bool(id obj, Ivar iv, BOOL v) { *(BOOL *)((char *)(__bridge void *)obj + ivar_getOffset(iv)) = v; }
static BOOL hidden_and_active(id ctl) { return ivar_bool(ctl, g_iv_wants_hidden) && [NSApp isActive]; }

static BOOL warp(CGPoint p)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CGSetLocalEventsSuppressionInterval(0);   // as Wine does: no 0.25 s input freeze after the warp
    CGError e = CGWarpMouseCursorPosition(p);
    CGSetLocalEventsSuppressionInterval(0.25);
#pragma clang diagnostic pop
    return e == kCGErrorSuccess;
}

static void leave(id ctl, const char *why)
{
    if (!g_rel) return;
    CGAssociateMouseAndMouseCursorPosition(true);
    g_rel = NO;
    g_same_sets = 0;
    // the next move is sent as an absolute position, which re-syncs Windows' cursor with the real one
    if (g_iv_force_abs) set_ivar_bool(ctl, g_iv_force_abs, YES);
    tf2mt_log("relmouse: off (%s); relative events %llu, re-centre warps skipped %llu\n", why, g_rel_events, g_warps_skipped);
}

static BOOL enter(CGPoint at)
{
    if (CGAssociateMouseAndMouseCursorPosition(false) != kCGErrorSuccess) { tf2mt_log("relmouse: disassociate failed\n"); return NO; }
    if (!warp(at)) { CGAssociateMouseAndMouseCursorPosition(true); return NO; }
    g_rel = YES; g_park = at; g_entries++;
    tf2mt_log("relmouse: on (cursor parked at %.0f,%.0f), entry %llu\n", at.x, at.y, g_entries);
    return YES;
}

static void hook_handle_mouse_move(id self, SEL cmd, NSEvent *event)
{
    if (g_rel && !hidden_and_active(self)) leave(self, "cursor shown or app inactive");
    NSMutableData *rects = g_rel ? object_getIvar(self, g_iv_screen_rects) : nil;
    if (!rects) { orig_handle_mouse_move(self, cmd, event); return; }
    // Wine decides absolute vs relative after making sure the screen cache is valid; with it valid and the rect list
    // empty during the call, the motion counts as "pinned" and goes out as relative deltas
    ((CGFloat (*)(id, SEL))objc_msgSend)(self, @selector(primaryScreenHeight));
    static uint8_t saved[64 * sizeof(CGRect)];
    NSUInteger len = rects.length;
    if (len > sizeof saved) { orig_handle_mouse_move(self, cmd, event); return; }
    memcpy(saved, rects.bytes, len);
    rects.length = 0;
    orig_handle_mouse_move(self, cmd, event);
    if (object_getIvar(self, g_iv_screen_rects) == rects && rects.length == 0) { rects.length = len; memcpy(rects.mutableBytes, saved, len); }
    g_rel_events++;
}

static BOOL hook_set_cursor_position(id self, SEL cmd, CGPoint pos)
{
    if (!hidden_and_active(self)) { leave(self, "cursor shown or app inactive"); g_same_sets = 0; return orig_set_cursor_position(self, cmd, pos); }
    if (g_rel) {
        if (pos.x == g_park.x && pos.y == g_park.y) { g_warps_skipped++; return YES; }   // the per-frame re-centre: no warp
        if (!warp(pos)) return NO;   // a new centre (window moved or resized): park there
        g_park = pos;
        return YES;
    }
    double now = CACurrentMediaTime();
    g_same_sets = (pos.x == g_last_set.x && pos.y == g_last_set.y && now - g_last_set_t < 1.0) ? g_same_sets + 1 : 1;
    g_last_set = pos; g_last_set_t = now;
    if (g_same_sets >= 2 && enter(pos)) return YES;
    return orig_set_cursor_position(self, cmd, pos);
}

static void hook_unhide_cursor(id self, SEL cmd)
{
    orig_unhide_cursor(self, cmd);
    leave(self, "cursor shown");
}

static IMP swap(Class c, SEL s, IMP repl)
{
    Method m = class_getInstanceMethod(c, s);
    return m ? method_setImplementation(m, repl) : NULL;
}

static void install(const char *who)
{
    const char *v = getenv("TF2MT_RELMOUSE");
    if (v && v[0] == '0') { tf2mt_log("relmouse: disabled (TF2MT_RELMOUSE=0)\n"); return; }
    Class c = objc_getClass("WineApplicationController");
    if (!c) { tf2mt_log("relmouse: no WineApplicationController (not the Mac driver?)\n"); return; }
    // one install per process (tf2mt.so and relmouse.dylib may both be loaded): selectors are process-wide keys
    SEL marker = sel_registerName("tf2mt_relmouse_installed");
    @synchronized (c) {
        if (objc_getAssociatedObject(c, marker)) { tf2mt_log("relmouse: already installed (%s skipped)\n", who); return; }
        g_iv_wants_hidden = class_getInstanceVariable(c, "clientWantsCursorHidden");
        g_iv_screen_rects = class_getInstanceVariable(c, "screenFrameCGRects");
        g_iv_force_abs = class_getInstanceVariable(c, "forceNextMouseMoveAbsolute");
        SEL need[] = {@selector(handleMouseMove:), @selector(setCursorPosition:), @selector(unhideCursor),
                      @selector(primaryScreenHeight)};
        for (size_t i = 0; i < sizeof need / sizeof need[0]; i++)
            if (!class_getInstanceMethod(c, need[i])) { tf2mt_log("relmouse: Wine build lacks %s, not installed\n", sel_getName(need[i])); return; }
        if (!class_getClassMethod(c, @selector(sharedController))) { tf2mt_log("relmouse: Wine build lacks +sharedController, not installed\n"); return; }
        if (!g_iv_wants_hidden || !g_iv_screen_rects) { tf2mt_log("relmouse: Wine build lacks the cursor ivars, not installed\n"); return; }
        g_ctl_class = c;
        orig_handle_mouse_move = (void (*)(id, SEL, NSEvent *))swap(c, @selector(handleMouseMove:), (IMP)hook_handle_mouse_move);
        orig_set_cursor_position = (BOOL (*)(id, SEL, CGPoint))swap(c, @selector(setCursorPosition:), (IMP)hook_set_cursor_position);
        orig_unhide_cursor = (void (*)(id, SEL))swap(c, @selector(unhideCursor), (IMP)hook_unhide_cursor);
        objc_setAssociatedObject(c, marker, @YES, OBJC_ASSOCIATION_RETAIN);
    }
    // the app losing focus (Cmd-Tab, a notification click) must give the mouse back at once
    [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidResignActiveNotification object:nil
                                                       queue:nil usingBlock:^(NSNotification *n) {
        id ctl = ((id (*)(id, SEL))objc_msgSend)((id)g_ctl_class, @selector(sharedController));
        if (ctl) leave(ctl, "app inactive");
    }];
    tf2mt_log("relmouse: installed (%s)\n", who);
}

#ifdef RELMOUSE_DYLIB
// Loaded into every process the game launch starts (Wine loader, tf.exe, tf_win64.exe). Only processes that load the
// Mac driver get the hooks; installing runs on the main thread (Wine's Cocoa thread) once its run loop is up.
static void image_added(const struct mach_header *mh, intptr_t slide)
{
    static int done;
    Dl_info info;
    if (done || !dladdr(mh, &info) || !info.dli_fname || !strstr(info.dli_fname, "/winemac.so")) return;
    done = 1;
    dispatch_async(dispatch_get_main_queue(), ^{ @autoreleasepool { install("relmouse.dylib"); } });
}
__attribute__((constructor)) static void relmouse_dylib_init(void)
{
    const char *v = getenv("TF2MT_RELMOUSE");
    if (v && v[0] == '0') return;
    _dyld_register_func_for_add_image(image_added);
}
#else
void tf2mt_relmouse_install(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{ install("tf2mt.so"); });
}
#endif
