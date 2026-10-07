/* displaystate — is the main display awake and the session unlocked? (benchmark validity guard)
 * Prints "awake=<0|1> locked=<0|1> hz=<refresh>" and exits 0 only if awake && !locked.
 * A locked screen or sleeping display stops WindowServer composition, which changes presentation
 * behaviour completely (runs taken that way are invalid — see docs/phase0-report.md).
 */
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>

int main(void)
{
    CGDirectDisplayID d = CGMainDisplayID();
    int awake = !CGDisplayIsAsleep(d);
    int locked = 0;
    CFDictionaryRef s = CGSessionCopyCurrentDictionary();
    if (s) {
        CFBooleanRef v = CFDictionaryGetValue(s, CFSTR("CGSSessionScreenIsLocked"));
        locked = v && CFBooleanGetValue(v);
        CFRelease(s);
    }
    double hz = 0;
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(d);
    if (m) { hz = CGDisplayModeGetRefreshRate(m); CGDisplayModeRelease(m); }
    printf("awake=%d locked=%d hz=%.0f\n", awake, locked, hz);
    return awake && !locked ? 0 : 1;
}
