// SPDX-License-Identifier: Apache-2.0
// upright <in> <out.png> [seconds] — writes the image as a viewer shows it. A photo is decoded with its EXIF
// orientation applied; for a movie, the frame at <seconds> (default 0.8) is taken with the track's preferredTransform
// applied. Prints the image's size.
import Foundation
import AVFoundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count > 2 else {
  FileHandle.standardError.write(Data("usage: upright <in> <out.png> [seconds]\n".utf8))
  exit(2)
}

let defaultSeconds = 0.8
// a photo is scaled to fit this many pixels on its longer side
let maxPhotoSide = 1600
// the timescale the movie frame's time is given in
let frameTimescale: CMTimeScale = 600

let input = URL(fileURLWithPath: args[1])
let output = URL(fileURLWithPath: args[2])
guard let seconds = args.count > 3 ? Double(args[3]) : defaultSeconds else {
  print("ERROR seconds must be a number, not \(args[3])")
  exit(1)
}

var image: CGImage?
if ["mov", "mp4", "m4v"].contains(input.pathExtension.lowercased()) {
  let generator = AVAssetImageGenerator(asset: AVURLAsset(url: input))
  generator.appliesPreferredTrackTransform = true
  generator.requestedTimeToleranceBefore = .zero
  generator.requestedTimeToleranceAfter = .positiveInfinity   // the first frame at or after the time asked for
  image = try? generator.copyCGImage(at: CMTime(seconds: seconds, preferredTimescale: frameTimescale), actualTime: nil)
} else if let source = CGImageSourceCreateWithURL(input as CFURL, nil) {
  let options: [CFString: Any] = [
    kCGImageSourceCreateThumbnailFromImageAlways: true,
    kCGImageSourceCreateThumbnailWithTransform: true,
    kCGImageSourceThumbnailMaxPixelSize: maxPhotoSide,
  ]
  image = CGImageSourceCreateThumbnailAtIndex(source, 0, options as CFDictionary)
}
guard let image else {
  print("ERROR cannot decode \(input.path)")
  exit(1)
}

let pngType = UTType.png.identifier as CFString
guard let destination = CGImageDestinationCreateWithURL(output as CFURL, pngType, 1, nil) else {
  print("ERROR cannot write \(output.path)")
  exit(1)
}
CGImageDestinationAddImage(destination, image, nil)
guard CGImageDestinationFinalize(destination) else {
  print("ERROR cannot write \(output.path)")
  exit(1)
}
print("\(image.width)x\(image.height)")
