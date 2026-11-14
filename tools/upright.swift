// SPDX-License-Identifier: Apache-2.0
// upright <in> <out.png> [seconds] — photo: decode with EXIF orientation applied; movie: frame at t with preferredTransform applied (as a viewer shows it)
import Foundation; import AVFoundation; import ImageIO; import CoreGraphics; import UniformTypeIdentifiers
let a = CommandLine.arguments; let inU = URL(fileURLWithPath: a[1]); let outU = URL(fileURLWithPath: a[2]); let t = a.count > 3 ? Double(a[3])! : 0.8
var img: CGImage?
if ["mov", "mp4", "m4v"].contains(inU.pathExtension.lowercased()) {
  let g = AVAssetImageGenerator(asset: AVURLAsset(url: inU)); g.appliesPreferredTrackTransform = true; g.requestedTimeToleranceBefore = .zero; g.requestedTimeToleranceAfter = .positiveInfinity
  img = try? g.copyCGImage(at: CMTime(seconds: t, preferredTimescale: 600), actualTime: nil)
} else if let src = CGImageSourceCreateWithURL(inU as CFURL, nil) {
  img = CGImageSourceCreateThumbnailAtIndex(src, 0, [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceCreateThumbnailWithTransform: true, kCGImageSourceThumbnailMaxPixelSize: 1600] as CFDictionary)
}
guard let img else { print("ERROR cannot decode \(inU.path)"); exit(1) }
let d = CGImageDestinationCreateWithURL(outU as CFURL, UTType.png.identifier as CFString, 1, nil)!; CGImageDestinationAddImage(d, img, nil); CGImageDestinationFinalize(d)
print("\(img.width)x\(img.height)")
