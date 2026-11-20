// Mouse motion through the HID event system (IOHIDPostEvent, relative), the closest thing to a real mouse that needs no
// driver: macOS moves the cursor by the delta only while the mouse is associated with the cursor
// (CGAssociateMouseAndMouseCursorPosition), exactly like hardware. Synthetic CGEvents (mousedrive) carry an absolute
// position and always move the cursor, which hides the effect of relative mouse mode (issue #2).
//   hiddrive <rate_hz> <dx> <phases> [dy]   phases: on:<s>,off:<s>,... (direction flips every second)
// Prints "phase <kind> <start_ms> <secs>" (mach ms, same clock as tf2mt's logs) like mousedrive.
#include <IOKit/IOKitLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/hidsystem/event_status_driver.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static double now_ms(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: hiddrive <hz> <dx> <phases>\n"); return 2; }
    double hz = atof(argv[1]); int dx = atoi(argv[2]), dy = argc > 4 ? atoi(argv[4]) : 0;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
    io_connect_t c = 0;
    if (!s || IOServiceOpen(s, mach_task_self(), kIOHIDParamConnectType, &c) != KERN_SUCCESS) { fprintf(stderr, "no IOHIDSystem\n"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sign = 1;
    char *list = strdup(argv[3]);
    for (char *ph = strtok(list, ","); ph; ph = strtok(NULL, ",")) {
        char kind[16] = ""; double secs = 1;
        sscanf(ph, "%15[^:]:%lf", kind, &secs);
        int on = strcmp(kind, "on") == 0;
        double t0 = now_ms();
        printf("phase %s %.3f %g\n", kind, t0, secs);
        for (long n = 0; now_ms() - t0 < secs * 1000; n++) {
            if (on) {
                if (n % (long)hz == 0) sign = -sign;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                NXEventData d; memset(&d, 0, sizeof d);
                d.mouseMove.dx = dx * sign; d.mouseMove.dy = dy * sign;
                IOGPoint loc = {0, 0};
                IOHIDPostEvent(c, NX_MOUSEMOVED, loc, &d, kNXEventDataVersion, 0, kIOHIDSetRelativeCursorPosition);
#pragma clang diagnostic pop
            }
            double wait = t0 + (n + 1) * 1000.0 / hz - now_ms();
            if (wait > 0) usleep((useconds_t)(wait * 1000));
        }
    }
    printf("done %.3f\n", now_ms());
    IOServiceClose(c);
    return 0;
}
