// SPDX-License-Identifier: Apache-2.0
// avinfo <file> — for a photo: its size, EXIF orientation and type. For a movie: its duration, then for each track its
// type, codec, size, rotation and nominal frame rate.
import AVFoundation
import ImageIO
import Foundation

guard CommandLine.arguments.count > 1 else {
  FileHandle.standardError.write(Data("usage: avinfo <file>\n".utf8))
  exit(2)
}

let url = URL(fileURLWithPath: CommandLine.arguments[1])

// a photo: its pixel size, EXIF orientation and type
if ["jpg", "jpeg", "heic", "png"].contains(url.pathExtension.lowercased()) {
  if let source = CGImageSourceCreateWithURL(url as CFURL, nil),
     let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [String: Any] {
    let type = CGImageSourceGetType(source) ?? "?" as CFString
    let size = "\(properties["PixelWidth"] ?? "?")x\(properties["PixelHeight"] ?? "?")"
    print("image \(size) orientation=\(properties["Orientation"] ?? "nil") type=\(type)")
  } else {
    print("image UNREADABLE")
  }
  exit(0)
}

// a track's codec as its four-character code, or "?"
func codecName(_ formats: [CMFormatDescription]) -> String {
  guard let format = formats.first else { return "?" }
  // split up: the one-expression form timed out in Xcode 26.6's type checker
  let subtype: FourCharCode = CMFormatDescriptionGetMediaSubType(format)
  let bytes: [UInt8] = [
    UInt8((subtype >> 24) & 255),
    UInt8((subtype >> 16) & 255),
    UInt8((subtype >> 8) & 255),
    UInt8(subtype & 255),
  ]
  return String(bytes: bytes, encoding: .ascii) ?? "?"
}

// a movie: its duration, then for each track its type, codec, size, rotation and nominal frame rate
let asset = AVURLAsset(url: url)
let done = DispatchSemaphore(value: 0)
Task {
  do {
    let duration = try await asset.load(.duration)
    let tracks = try await asset.load(.tracks)
    var line = "video duration=\(String(format: "%.2f", duration.seconds))s tracks=\(tracks.count)"
    for track in tracks {
      let size = try await track.load(.naturalSize)
      let transform = try await track.load(.preferredTransform)
      let codec = codecName(try await track.load(.formatDescriptions))
      let rotationDegrees = Int(atan2(transform.b, transform.a) * 180 / Double.pi)
      let rate = try await track.load(.nominalFrameRate)
      line += " [\(track.mediaType.rawValue) \(codec) \(Int(size.width))x\(Int(size.height)) rot=\(rotationDegrees)"
        + " frames~\(rate)fps]"
    }

    print(line)
  } catch {
    print("video ERROR \(error)")
  }
  done.signal()
}
done.wait()
