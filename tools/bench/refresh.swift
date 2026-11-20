// Main display refresh rate for tests (issue #2: fps lock at the display rate on 60 Hz panels).
//   refresh list            list modes with the current resolution
//   refresh get             print the current refresh rate
//   refresh set <hz>        switch to <hz> at the current resolution (session-only, macOS restores it on logout)
import CoreGraphics
import Foundation
let d = CGMainDisplayID()
guard let cur = CGDisplayCopyDisplayMode(d) else { exit(1) }
let opts = [kCGDisplayShowDuplicateLowResolutionModes: kCFBooleanTrue] as CFDictionary
let modes = (CGDisplayCopyAllDisplayModes(d, opts) as? [CGDisplayMode] ?? [])
    .filter { $0.width == cur.width && $0.height == cur.height && $0.pixelWidth == cur.pixelWidth }
let a = CommandLine.arguments
switch a.count > 1 ? a[1] : "get" {
case "list": for m in modes { print("\(m.width)x\(m.height) px \(m.pixelWidth)x\(m.pixelHeight) @ \(m.refreshRate) Hz") }
case "get": print(cur.refreshRate)
case "set":
    guard a.count > 2, let hz = Double(a[2]) else { print("usage: refresh set <hz>"); exit(2) }
    guard let m = modes.min(by: { abs($0.refreshRate - hz) < abs($1.refreshRate - hz) }), abs(m.refreshRate - hz) < 1 else {
        print("no \(hz) Hz mode"); exit(3) }
    var cfg: CGDisplayConfigRef?
    CGBeginDisplayConfiguration(&cfg)
    CGConfigureDisplayWithDisplayMode(cfg, d, m, nil)
    let e = CGCompleteDisplayConfiguration(cfg, .forSession)
    print(e == .success ? "set \(m.refreshRate) Hz" : "failed \(e.rawValue)")
default: print("usage: refresh list|get|set <hz>")
}
