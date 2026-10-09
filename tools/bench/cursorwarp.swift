// Simulates TF2's per-frame cursor recentring (SetCursorPos -> CGWarpMouseCursorPosition in Wine) for latency tests.
// usage: cursorwarp <hz> <seconds> [events]   ("events" also posts synthetic mouse-moved events, needs Accessibility)
import CoreGraphics
import Foundation
let a = CommandLine.arguments
let hz = a.count > 1 ? Double(a[1]) ?? 120 : 120, secs = a.count > 2 ? Double(a[2]) ?? 60 : 60
let events = a.count > 3 && a[3] == "events"
let b = CGDisplayBounds(CGMainDisplayID())
let c = CGPoint(x: b.midX, y: b.midY)
let end = Date().addingTimeInterval(secs)
var i = 0
while Date() < end {
    let p = CGPoint(x: c.x + CGFloat(i % 2 == 0 ? 3 : -3), y: c.y)
    if events, let e = CGEvent(mouseEventSource: nil, mouseType: .mouseMoved, mouseCursorPosition: p, mouseButton: .left) { e.post(tap: .cghidEventTap) }
    CGWarpMouseCursorPosition(c)
    i += 1
    usleep(useconds_t(1_000_000 / hz))
}
