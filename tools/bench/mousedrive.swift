// Synthetic mouse motion for input/pacing tests (issue #2). Posts relative mouse-moved events (delta fields set, like
// a real mouse) at the HID tap. Needs Accessibility permission for the calling process.
//   mousedrive <rate_hz> <dx> <phases>      phases: comma list of on:<s> / off:<s>, e.g. off:8,on:8,off:8,on:8
// Prints "phase <name> <start_ms>" on stdout with mach-based ms (same clock as tf2mt's ms_now / presentedTime).
import AppKit
import CoreGraphics
import Foundation
var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
func nowMs() -> Double { Double(mach_absolute_time()) * Double(tb.numer) / Double(tb.denom) / 1e6 }
let a = CommandLine.arguments
//   mousedrive click <x> <y>                left click at a screen point (menus in test setup)
if a.count == 4, a[1] == "click", let x = Double(a[2]), let y = Double(a[3]) {
    let p = CGPoint(x: x, y: y)
    for t in [CGEventType.mouseMoved, .leftMouseDown, .leftMouseUp] {
        CGEvent(mouseEventSource: nil, mouseType: t, mouseCursorPosition: p, mouseButton: .left)?.post(tap: .cghidEventTap)
        usleep(60000)
    }
    exit(0)
}
//   mousedrive pos                          print the on-screen cursor position "x y" (WindowServer's, global coordinates)
if a.count == 2, a[1] == "pos" { let l = NSEvent.mouseLocation; print("\(Int(l.x)) \(Int(l.y))"); exit(0) }
//   mousedrive down|up                      left button down / up at the cursor (shooting while turning)
if a.count == 2, a[1] == "down" || a[1] == "up" {
    let l = CGEvent(source: nil)?.location ?? .zero
    CGEvent(mouseEventSource: nil, mouseType: a[1] == "down" ? .leftMouseDown : .leftMouseUp, mouseCursorPosition: l, mouseButton: .left)?.post(tap: .cghidEventTap)
    exit(0)
}
//   mousedrive key <keycode>                press and release a key (53 = Escape)
if a.count == 3, a[1] == "key", let k = UInt16(a[2]) {
    for down in [true, false] { CGEvent(keyboardEventSource: nil, virtualKey: k, keyDown: down)?.post(tap: .cghidEventTap); usleep(60000) }
    exit(0)
}
guard a.count > 3, let hz = Double(a[1]), let dx = Int64(a[2]) else { print("usage: mousedrive <hz> <dx> <phases>"); exit(2) }
let src = CGEventSource(stateID: .hidSystemState)
setvbuf(stdout, nil, _IOLBF, 0)
var sign: Int64 = 1
for ph in a[3].split(separator: ",") {
    let kv = ph.split(separator: ":"); let kind = String(kv[0]); let on = kind == "on"; let secs = Double(kv[1]) ?? 1
    // kinds: on = mouse motion (delta + cursor position), off = nothing, warp = CGWarpMouseCursorPosition only (what Wine
    // does for SetCursorPos), move = cursor position only (no delta fields), delta = delta fields only (position kept)
    let t0 = nowMs(); print("phase \(kind) \(String(format: "%.3f", t0)) \(secs)")
    var n = 0
    while nowMs() - t0 < secs * 1000 {
        if on {
            // alternate direction every second so the view oscillates instead of spinning
            if n % Int(hz) == 0 { sign = -sign }
            // like a real mouse: the cursor position moves by the delta too (Wine ignores delta-only events)
            let cur = CGEvent(source: nil)?.location ?? .zero
            let loc = CGPoint(x: cur.x + CGFloat(dx * sign), y: cur.y)
            if let e = CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: loc, mouseButton: .left) {
                e.setIntegerValueField(.mouseEventDeltaX, value: dx * sign)
                e.setIntegerValueField(.mouseEventDeltaY, value: 0)
                e.post(tap: .cghidEventTap)
            }
            n += 1
        } else if kind != "off" {
            if n % Int(hz) == 0 { sign = -sign }
            let cur = CGEvent(source: nil)?.location ?? .zero
            let p = CGPoint(x: cur.x + CGFloat(dx * sign), y: cur.y)
            if kind == "warp" { CGWarpMouseCursorPosition(p) }
            else if let e = CGEvent(mouseEventSource: src, mouseType: .mouseMoved, mouseCursorPosition: kind == "move" ? p : cur, mouseButton: .left) {
                e.setIntegerValueField(.mouseEventDeltaX, value: kind == "delta" ? dx * sign : 0)
                e.post(tap: .cghidEventTap)
            }
            n += 1
        }
        // absolute schedule: no drift from post cost
        let next = t0 + Double(n + 1) * 1000.0 / hz
        let wait = next - nowMs(); if wait > 0 { usleep(useconds_t(wait * 1000)) }
    }
}
print("done \(String(format: "%.3f", nowMs()))")
