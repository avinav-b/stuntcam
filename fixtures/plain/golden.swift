// SPDX-License-Identifier: Apache-2.0
// M5APP_MODE=golden: hashes of the frames the engine delivers, for scripts/golden.sh. Opens the back (or front) camera
// at one format, sets the video data connection's rotation and mirroring, and hashes every frame's pixels (each plane's
// visible rows, without row padding) for GF_SECS after 1 s of warm-up; optionally also a photo's decoded pixels, and a
// GF_MOVIE-second movie (AVCaptureMovieFileOutput) whose frames are decoded and hashed the same way.
// Env: GF_W / GF_H (1920x1080), GF_FPS (30), GF_FMT bgra|420v|420f, GF_POS back|front, GF_ROT 0|90|180|270,
// GF_MIRROR 0|1, GF_SECS (8), GF_PHOTO 0|1, GF_MOVIE <seconds> (0: none).
// Prints GFH <hash> <count> for each distinct frame, GFPHOTO <hash>, GFMOVIE <index> <hash> for the movie's first 48
// frames, GFSTAT, GFMOVIESTAT, then DONE.
import UIKit
import AVFoundation
import CryptoKit

func gfHash(_ pb: CVPixelBuffer) -> String {
  CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
  var h = SHA256()
  let f = CVPixelBufferGetPixelFormatType(pb)
  withUnsafeBytes(of: f.bigEndian) { h.update(bufferPointer: $0) }
  if CVPixelBufferIsPlanar(pb) {
    for p in 0..<CVPixelBufferGetPlaneCount(pb) {
      let b = CVPixelBufferGetBaseAddressOfPlane(pb, p)!, bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, p)
      let w = CVPixelBufferGetWidthOfPlane(pb, p), rows = CVPixelBufferGetHeightOfPlane(pb, p), bpp = p == 0 ? 1 : 2
      for y in 0..<rows { h.update(bufferPointer: UnsafeRawBufferPointer(start: b + y * bpr, count: w * bpp)) }
    }
  } else {
    let b = CVPixelBufferGetBaseAddress(pb)!, bpr = CVPixelBufferGetBytesPerRow(pb), w = CVPixelBufferGetWidth(pb)
    for y in 0..<CVPixelBufferGetHeight(pb) { h.update(bufferPointer: UnsafeRawBufferPointer(start: b + y * bpr, count: w * 4)) }
  }
  return h.finalize().prefix(10).map { String(format: "%02x", $0) }.joined() + "-\(CVPixelBufferGetWidth(pb))x\(CVPixelBufferGetHeight(pb))"
}
func gfHash(_ img: CGImage) -> String {
  let w = img.width, h = img.height, bpr = w * 4
  var buf = [UInt8](repeating: 0, count: bpr * h)
  _ = buf.withUnsafeMutableBytes { p in
    CGContext(data: p.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr, space: CGColorSpaceCreateDeviceRGB(),
              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)?.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h)) }
  return SHA256.hash(data: buf).prefix(10).map { String(format: "%02x", $0) }.joined() + "-\(w)x\(h)"
}

final class GF: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate {
  let lock = NSLock(); var counting = false, frames = 0, counts: [String: Int] = [:], photoCB: ((AVCapturePhoto?) -> Void)?, movieCB: ((URL, Error?) -> Void)?
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    guard let pb = CMSampleBufferGetImageBuffer(sb) else { return }
    lock.lock(); let on = counting; lock.unlock()
    if !on { return }
    let k = gfHash(pb)
    lock.lock(); if counting { frames += 1; counts[k, default: 0] += 1 }; lock.unlock()
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto p: AVCapturePhoto, error: Error?) { photoCB?(p) }
  func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo u: URL, from c: [AVCaptureConnection], error: Error?) { movieCB?(u, error) }
}
// the movie at u, decoded to BGRA: the hashes of its first gfMovieFrames frames, by index. The encoder's output for the
// same input frames is the same, but its P-frames refine a still picture frame by frame, so the set of distinct frames
// depends on the movie's length (61 or 62 frames in 2 s); the first 48 don't.
let gfMovieFrames = 48
func gfMovie(_ u: URL, _ e: Error?) {
  let a = AVURLAsset(url: u)
  guard let tr = a.tracks(withMediaType: .video).first, let r = try? AVAssetReader(asset: a) else { R(false, "gf.movie", "no track err=\(String(describing: e))"); return }
  let o = AVAssetReaderTrackOutput(track: tr, outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]); r.add(o); r.startReading()
  var counts: [String: Int] = [:], n = 0
  while let sb = o.copyNextSampleBuffer() { if let pb = CMSampleBufferGetImageBuffer(sb) { let k = gfHash(pb); if n < gfMovieFrames { P(String(format: "GFMOVIE %02d %@", n, k)) }; n += 1; counts[k, default: 0] += 1 } }
  P("GFMOVIESTAT frames=\(n) distinct=\(counts.count) size=\(Int(tr.naturalSize.width))x\(Int(tr.naturalSize.height)) err=\(String(describing: e))")
  R(n >= gfMovieFrames, "gf.movie", "\(n) frames")
}
var gfKeep: GF?

extension VC {
  func golden() {
    let env = ProcessInfo.processInfo.environment
    let W = Int32(env["GF_W"] ?? "1920")!, H = Int32(env["GF_H"] ?? "1080")!, fps = Double(env["GF_FPS"] ?? "30")!, secs = Double(env["GF_SECS"] ?? "8")!
    let front = env["GF_POS"] == "front", rot = CGFloat(Double(env["GF_ROT"] ?? "0")!), mirror = env["GF_MIRROR"] == "1", wantPhoto = env["GF_PHOTO"] == "1"
    let movieSecs = Double(env["GF_MOVIE"] ?? "0")!
    let pf: OSType = env["GF_FMT"] == "420v" ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : env["GF_FMT"] == "420f" ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_32BGRA
    guard let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: front ? .front : .back) else { R(false, "gf.device"); done(); return }
    let m = GF(); gfKeep = m
    let s = session
    s.beginConfiguration()
    guard let inp = try? AVCaptureDeviceInput(device: dev), s.canAddInput(inp) else { R(false, "gf.input"); done(); return }
    s.addInput(inp)
    let vdo = AVCaptureVideoDataOutput(); vdo.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: pf]; vdo.alwaysDiscardsLateVideoFrames = false
    vdo.setSampleBufferDelegate(m, queue: DispatchQueue(label: "gf.vdo", qos: .userInteractive))
    if s.canAddOutput(vdo) { s.addOutput(vdo) }
    let po = AVCapturePhotoOutput(); if wantPhoto, s.canAddOutput(po) { s.addOutput(po) }
    let mo = AVCaptureMovieFileOutput(); if movieSecs > 0, s.canAddOutput(mo) { s.addOutput(mo) }
    if let c = vdo.connection(with: .video) {
      if c.isVideoRotationAngleSupported(rot) { c.videoRotationAngle = rot } else { R(false, "gf.rotation", "\(rot) unsupported") }
      if mirror, c.isVideoMirroringSupported { c.automaticallyAdjustsVideoMirroring = false; c.isVideoMirrored = true }
    }
    s.commitConfiguration()
    if let f = dev.formats.first(where: { let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription); return d.width == W && d.height == H && $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= fps } }) {
      do { try dev.lockForConfiguration(); dev.activeFormat = f; dev.activeVideoMinFrameDuration = CMTime(value: 1, timescale: CMTimeScale(fps)); dev.activeVideoMaxFrameDuration = dev.activeVideoMinFrameDuration; dev.unlockForConfiguration()
        P("INFO gf.format \(fmtDesc(f)) fps=\(fps) pos=\(front ? "front" : "back") rot=\(rot) mirror=\(mirror)") } catch { R(false, "gf.format", "\(error)") }
    } else { R(false, "gf.format", "no \(W)x\(H)@\(fps)") }
    DispatchQueue.global().async {
      s.startRunning()
      P("INFO gf.running \(s.isRunning)")
      DispatchQueue.global().asyncAfter(deadline: .now() + 1.0) {
        m.lock.lock(); m.counting = true; m.lock.unlock()
        DispatchQueue.global().asyncAfter(deadline: .now() + secs) {
          m.lock.lock(); m.counting = false; let counts = m.counts, n = m.frames; m.lock.unlock()
          for (k, v) in counts.sorted(by: { $0.key < $1.key }) { P("GFH \(k) \(v)") }
          P("GFSTAT frames=\(n) distinct=\(counts.count)")
          let end = { s.stopRunning(); self.done() }
          let finish = {   // then the movie, if any
            guard movieSecs > 0, !mo.connections.isEmpty else { end(); return }
            let url = FileManager.default.temporaryDirectory.appendingPathComponent("gf-\(getpid()).mov"); try? FileManager.default.removeItem(at: url)
            m.movieCB = { u, e in DispatchQueue.global().async { gfMovie(u, e); end() } }
            mo.startRecording(to: url, recordingDelegate: m)
            DispatchQueue.global().asyncAfter(deadline: .now() + movieSecs) { mo.stopRecording() }
          }
          guard wantPhoto, !po.connections.isEmpty else { finish(); return }
          m.photoCB = { p in
            if let img = p?.cgImageRepresentation() { P("GFPHOTO \(gfHash(img))") } else { R(false, "gf.photo") }
            finish()
          }
          po.capturePhoto(with: AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg]), delegate: m)
        }
      }
    }
  }
}
