// SPDX-License-Identifier: Apache-2.0
// mkvideo <out.mov> [fps frames] — writes a 1280x720 H.264 movie (by default 30 fps and 120 frames, which is 4 s): a
// white block moving left to right across a magenta background over the length of the clip. Prints the writer's final
// status (2 = completed).
import AVFoundation
import CoreGraphics

func die(_ message: String) -> Never {
  FileHandle.standardError.write(Data("mkvideo: \(message)\n".utf8))
  exit(1)
}

let args = CommandLine.arguments
guard args.count > 1 else {
  FileHandle.standardError.write(Data("usage: mkvideo <out.mov> [fps frames]\n".utf8))
  exit(2)
}

let defaultFPS: Int32 = 30
let defaultFrameCount = 120
let width = 1280
let height = 720
// the white block's side, in pixels
let blockSide = 200.0
let backgroundColor = CGColor(red: 0.8, green: 0.1, blue: 0.7, alpha: 1)   // magenta
let blockColor = CGColor(red: 1, green: 1, blue: 1, alpha: 1)
// how long to wait before asking again whether the writer takes another frame
let writerPollMicroseconds: useconds_t = 1000

let out = URL(fileURLWithPath: args[1])
try? FileManager.default.removeItem(at: out)  // usually there is none; the writer fails if it stays
guard let fps = args.count > 2 ? Int32(args[2]) : defaultFPS else {
  die("fps must be a whole number, not \(args[2])")
}
guard let frameCount = args.count > 3 ? Int(args[3]) : defaultFrameCount else {
  die("frames must be a whole number, not \(args[3])")
}

func makeWriter() -> AVAssetWriter {
  do {
    return try AVAssetWriter(outputURL: out, fileType: .mov)
  } catch {
    die("cannot write \(out.path): \(error.localizedDescription)")
  }
}

let writer = makeWriter()
let input = AVAssetWriterInput(mediaType: .video, outputSettings: [
  AVVideoCodecKey: AVVideoCodecType.h264,
  AVVideoWidthKey: width,
  AVVideoHeightKey: height,
])
let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
  kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
  kCVPixelBufferWidthKey as String: width,
  kCVPixelBufferHeightKey as String: height,
])
writer.add(input)
writer.startWriting()
writer.startSession(atSourceTime: .zero)

// Frame `index` of `count`: the block's left edge moves from 0 towards width - blockSide across the clip, and the block
// is centred vertically.
func drawFrame(_ pixelBuffer: CVPixelBuffer, _ index: Int, of count: Int) {
  CVPixelBufferLockBaseAddress(pixelBuffer, [])
  defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, []) }
  let bitmapInfo = CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
  guard let context = CGContext(data: CVPixelBufferGetBaseAddress(pixelBuffer), width: width, height: height,
                                bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(pixelBuffer),
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: bitmapInfo) else {
    die("cannot draw into frame \(index)")
  }
  context.setFillColor(backgroundColor)
  context.fill(CGRect(x: 0, y: 0, width: width, height: height))
  context.setFillColor(blockColor)
  let left = Double(index) * (Double(width) - blockSide) / Double(count)
  context.fill(CGRect(x: left, y: (Double(height) - blockSide) / 2, width: blockSide, height: blockSide))
}

for index in 0..<frameCount {
  while !input.isReadyForMoreMediaData {
    usleep(writerPollMicroseconds)
  }
  guard let pool = adaptor.pixelBufferPool else {
    die("the writer has no pixel buffer pool (status \(writer.status.rawValue))")
  }

  var pixelBuffer: CVPixelBuffer?
  CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pixelBuffer)
  guard let pixelBuffer else { die("cannot get a pixel buffer for frame \(index)") }
  drawFrame(pixelBuffer, index, of: frameCount)
  adaptor.append(pixelBuffer, withPresentationTime: CMTime(value: Int64(index), timescale: fps))
}
input.markAsFinished()
let done = DispatchSemaphore(value: 0)
writer.finishWriting { done.signal() }
done.wait()
print(writer.status.rawValue)
