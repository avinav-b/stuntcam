// SPDX-License-Identifier: Apache-2.0
// avinfo <file>: prints duration, tracks, dimensions, codec, image dims/orientation.
import AVFoundation
import ImageIO
import Foundation
let p = CommandLine.arguments[1]; let url = URL(fileURLWithPath: p)
if ["jpg","jpeg","heic","png"].contains(url.pathExtension.lowercased()) {
  if let src = CGImageSourceCreateWithURL(url as CFURL, nil), let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [String: Any] {
    print("image \(props["PixelWidth"] ?? "?")x\(props["PixelHeight"] ?? "?") orientation=\(props["Orientation"] ?? "nil") type=\(CGImageSourceGetType(src) ?? "?" as CFString)")
  } else { print("image UNREADABLE") }
  exit(0)
}
let a = AVURLAsset(url: url)
let sem = DispatchSemaphore(value: 0)
Task {
  do {
    let d = try await a.load(.duration); let ts = try await a.load(.tracks)
    var s = "video duration=\(String(format: "%.2f", d.seconds))s tracks=\(ts.count)"
    for t in ts {
      let sz = try await t.load(.naturalSize); let tf = try await t.load(.preferredTransform)
      let fds = try await t.load(.formatDescriptions)
      var codec = "?"   // split up: the one-expression form timed out in Xcode 26.6's type checker
      if let f = fds.first {
        let st: FourCharCode = CMFormatDescriptionGetMediaSubType(f)
        let bytes: [UInt8] = [UInt8((st >> 24) & 255), UInt8((st >> 16) & 255), UInt8((st >> 8) & 255), UInt8(st & 255)]
        codec = String(bytes: bytes, encoding: .ascii) ?? "?"
      }
      s += " [\(t.mediaType.rawValue) \(codec) \(Int(sz.width))x\(Int(sz.height)) rot=\(Int(atan2(tf.b, tf.a)*180/Double.pi)) frames~\(try await t.load(.nominalFrameRate))fps]"
    }
    print(s)
  } catch { print("video ERROR \(error)") }
  sem.signal()
}
sem.wait()
