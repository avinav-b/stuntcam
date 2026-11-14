// SPDX-License-Identifier: Apache-2.0
// mkvideo <out.mov> — 4 s 1280x720 H.264: magenta background + white block moving left->right + frame number bar
import AVFoundation; import CoreGraphics
let out = URL(fileURLWithPath: CommandLine.arguments[1]); try? FileManager.default.removeItem(at: out)
let w = try! AVAssetWriter(outputURL: out, fileType: .mov)
let inp = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: 1280, AVVideoHeightKey: 720])
let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: inp, sourcePixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: 1280, kCVPixelBufferHeightKey as String: 720])
w.add(inp); w.startWriting(); w.startSession(atSourceTime: .zero)
for i in 0..<120 {
  while !inp.isReadyForMoreMediaData { usleep(1000) }
  var pb: CVPixelBuffer?; CVPixelBufferPoolCreatePixelBuffer(nil, ad.pixelBufferPool!, &pb)
  CVPixelBufferLockBaseAddress(pb!, [])
  let c = CGContext(data: CVPixelBufferGetBaseAddress(pb!), width: 1280, height: 720, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(pb!), space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
  c.setFillColor(CGColor(red: 0.8, green: 0.1, blue: 0.7, alpha: 1)); c.fill(CGRect(x: 0, y: 0, width: 1280, height: 720))
  c.setFillColor(CGColor(red: 1, green: 1, blue: 1, alpha: 1)); c.fill(CGRect(x: Double(i) * 9, y: 260, width: 200, height: 200))
  CVPixelBufferUnlockBaseAddress(pb!, [])
  ad.append(pb!, withPresentationTime: CMTime(value: Int64(i), timescale: 30))
}
inp.markAsFinished(); let s = DispatchSemaphore(value: 0); w.finishWriting { s.signal() }; s.wait(); print(w.status.rawValue)
