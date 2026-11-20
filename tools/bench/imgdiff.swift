// Fraction of pixels that differ between two images (same size), for test automation: did the camera turn?
//   imgdiff a.png b.png   -> prints e.g. 0.734
import CoreGraphics
import Foundation
import ImageIO
func load(_ p: String) -> (Int, Int, [UInt8])? {
    guard let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(s, 0, nil) else { return nil }
    let w = 320, h = 180
    var px = [UInt8](repeating: 0, count: w * h * 4)
    let ok = px.withUnsafeMutableBytes { b -> Bool in
        guard let c = CGContext(data: b.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
        c.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h)); return true
    }
    return ok ? (w, h, px) : nil
}
let a = CommandLine.arguments
guard a.count == 3, let x = load(a[1]), let y = load(a[2]) else { print("usage: imgdiff a.png b.png"); exit(2) }
var diff = 0
for i in stride(from: 0, to: x.2.count, by: 4) {
    if abs(Int(x.2[i]) - Int(y.2[i])) + abs(Int(x.2[i + 1]) - Int(y.2[i + 1])) + abs(Int(x.2[i + 2]) - Int(y.2[i + 2])) > 48 { diff += 1 }
}
print(String(format: "%.3f", Double(diff) / Double(x.0 * x.1)))
