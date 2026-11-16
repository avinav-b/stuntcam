// SPDX-License-Identifier: Apache-2.0
// M5APP_MODE=webcam: measures the live webcam source. Reads the 8x8 latency stamp the camera helper draws into
// synthetic (and, with --stamp, camera) frames (helper/shim.h: seq 20 bits | host time in 100 us units 28 bits | check
// 16 bits) from every video-data frame, then from a photo and from every frame of a movie. Latency is also taken from
// each frame's presentation time, which the webcam source sets to the camera's capture time.
// Env: WC_W / WC_H (format, default 1920x1080), WC_FPS (30), WC_SECS (10), WC_POS back|front, WC_FMT bgra|420v,
// WC_PHOTO 1|0, WC_MOVIE <seconds> (5; 0 = none), WC_PREVIEW 1|0, WC_ROT 0|90|180|270 and WC_MIRROR 1 (the video data
// connection's rotation and mirroring; default: as AVFoundation sets them), WC_CYCLES N (soak: N windows, each with
// its photo and movie, in one session).
// Prints WCSEC (one line per second: frames, stamped, new stamps, offline-band frames) and WCSTAT lines, then DONE.
import UIKit
import AVFoundation
import CoreMedia

func wcNowNs() -> UInt64 {
  var tb = mach_timebase_info_data_t(); mach_timebase_info(&tb)
  return mach_absolute_time() * UInt64(tb.numer) / UInt64(tb.denom)
}
func wcNow100us() -> UInt32 { UInt32(truncatingIfNeeded: wcNowNs() / 100_000) & 0xfffffff }
func wcCheck(_ s: UInt32, _ t: UInt32) -> UInt32 {
  var x = (s << 12) ^ (t &* 2654435761); x ^= x >> 15; x = x &* 0x2c1b3c6d; x ^= x >> 12; x = x &* 0x297a2d39; x ^= x >> 15
  return x & 0xffff
}
struct Stamp { let seq: UInt32; let t: UInt32 }
/// Finds the stamp under any of the 8 rotations/mirrors and any scale; the last geometry that worked is tried first.
final class StampDecoder {
  var lastFrac = 0.5, lastT = 0
  func decode(w: Int, h: Int, lum: (Int, Int) -> Int) -> Stamp? {
    if let s = tryAt(lastFrac, lastT, w, h, lum) { return s }
    var fracs = [0.5, 0.8889, 0.6667, 0.75]; var f = 0.30; while f <= 1.0 { fracs.append(f); f += 0.01 }
    for fr in fracs { for t in 0..<8 { if let s = tryAt(fr, t, w, h, lum) { lastFrac = fr; lastT = t; return s } } }
    return nil
  }
  private func tryAt(_ frac: Double, _ t: Int, _ w: Int, _ h: Int, _ lum: (Int, Int) -> Int) -> Stamp? {
    let side = frac * Double(min(w, h)), cx = Double(w) / 2, cy = Double(h) / 2
    var bits: UInt64 = 0
    for k in 0..<64 {
      var u = (Double(k % 8) + 0.5) / 8 - 0.5, v = (Double(k / 8) + 0.5) / 8 - 0.5
      if t >= 4 { u = -u }
      for _ in 0..<(t % 4) { (u, v) = (-v, u) }
      let x = Int(cx + u * side), y = Int(cy + v * side)
      var sum = 0, n = 0
      for dy in -1...1 { for dx in -1...1 { let xx = x + dx * 2, yy = y + dy * 2; if xx >= 0 && yy >= 0 && xx < w && yy < h { sum += lum(xx, yy); n += 1 } } }
      if n > 0 && sum / n > 128 { bits |= 1 << UInt64(k) }
    }
    let s = UInt32(bits & 0xfffff), tt = UInt32((bits >> 20) & 0xfffffff), c = UInt32((bits >> 48) & 0xffff)
    return c == wcCheck(s, tt) && !(s == 0 && tt == 0) ? Stamp(seq: s, t: tt) : nil
  }
  func decode(_ pb: CVPixelBuffer) -> Stamp? {
    CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
    let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), f = CVPixelBufferGetPixelFormatType(pb)
    if f == kCVPixelFormatType_32BGRA {
      let b = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self), bpr = CVPixelBufferGetBytesPerRow(pb)
      return decode(w: w, h: h) { x, y in let p = b + y * bpr + x * 4; return (Int(p[0]) + 2 * Int(p[1]) + Int(p[2])) / 4 }
    }
    let b = CVPixelBufferGetBaseAddressOfPlane(pb, 0)!.assumingMemoryBound(to: UInt8.self), bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
    return decode(w: w, h: h) { x, y in Int(b[y * bpr + x]) }
  }
  func decode(_ img: CGImage) -> Stamp? {
    let w = img.width, h = img.height, bpr = w * 4
    var buf = [UInt8](repeating: 0, count: bpr * h)
    let ok = buf.withUnsafeMutableBytes { p -> Bool in
      guard let c = CGContext(data: p.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: bpr, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
      c.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h)); return true }
    if !ok { return nil }
    return buf.withUnsafeBufferPointer { b in decode(w: w, h: h) { x, y in let i = y * bpr + x * 4; return (Int(b[i]) + 2 * Int(b[i + 1]) + Int(b[i + 2])) / 4 } }
  }
}
/// The engine's offline band (dark red, about 0.75/0.08/0.16) on a 64x36 grid of BGRA samples: 2 % or more is a band frame.
func wcHasBand(_ pb: CVPixelBuffer) -> Bool {
  guard CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA else { return false }
  CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
  let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
  let b = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
  var hit = 0, n = 0
  for j in 0..<36 { for i in 0..<64 {
    let p = b + (j * h / 36 + h / 72) * bpr + (i * w / 64 + w / 128) * 4
    let B = Int(p[0]), G = Int(p[1]), R = Int(p[2]); n += 1
    if R >= 150 && R <= 230 && G <= 55 && B >= 15 && B <= 80 { hit += 1 }
  } }
  return hit * 50 >= n
}
func lat100(_ now: UInt32, _ t: UInt32) -> Double { Double((now &- t) & 0xfffffff) / 10.0 }   // ms
func pct(_ a: [Double], _ p: Double) -> Double { if a.isEmpty { return -1 }; let s = a.sorted(); return s[min(s.count - 1, Int(Double(s.count - 1) * p))] }

final class WC: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate {
  let dec = StampDecoder(), lock = NSLock()
  var collecting = true, frames = 0, fails = 0, dups = 0, gaps = 0, skipped = 0, lastSeq: UInt32 = 0, uniq = Set<UInt32>(), lats: [Double] = []
  var ptsLats: [Double] = [], ptsBackwards = 0, bandFrames = 0
  var firstT = 0.0, lastT = 0.0, ptsPrev = CMTime.invalid, ptsGapsMs: [Double] = [], dims = ""
  var t0 = CACurrentMediaTime(), sec = 0, sFrames = 0, sStamped = 0, sNew = 0, sBand = 0, counting = false   // WCSEC after warm-up
  var photoCB: ((AVCapturePhoto?, Error?) -> Void)?, movieCB: ((URL, Error?) -> Void)?
  func flushSec(_ now: Double) {
    while now - t0 >= Double(sec + 1) {
      if counting { P("WCSEC t=\(sec) frames=\(sFrames) stamped=\(sStamped) new=\(sNew) band=\(sBand)") }
      sec += 1; sFrames = 0; sStamped = 0; sNew = 0; sBand = 0
    }
  }
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    let nowNs = wcNowNs(), now = UInt32(truncatingIfNeeded: nowNs / 100_000) & 0xfffffff, tnow = CACurrentMediaTime()
    guard let pb = CMSampleBufferGetImageBuffer(sb) else { return }
    lock.lock(); defer { lock.unlock() }
    if !collecting { return }
    flushSec(tnow)
    if dims.isEmpty { dims = "\(CVPixelBufferGetWidth(pb))x\(CVPixelBufferGetHeight(pb)) \(fcc(CVPixelBufferGetPixelFormatType(pb)))" }
    frames += 1; sFrames += 1; if firstT == 0 { firstT = tnow }; lastT = tnow
    let pts = CMSampleBufferGetPresentationTimeStamp(sb)
    if ptsPrev.isValid { ptsGapsMs.append(CMTimeGetSeconds(pts - ptsPrev) * 1000); if CMTimeCompare(pts, ptsPrev) <= 0 { ptsBackwards += 1 } }; ptsPrev = pts
    guard let s = dec.decode(pb) else { fails += 1; if wcHasBand(pb) { bandFrames += 1; sBand += 1 }; return }
    sStamped += 1
    if s.seq == lastSeq { dups += 1 } else if lastSeq != 0 && s.seq > lastSeq + 1 { gaps += 1; skipped += Int(s.seq - lastSeq - 1) }
    if s.seq != lastSeq {   // a frame's latency the first time the app sees it
      sNew += 1; lats.append(lat100(now, s.t))
      let ptsNs = CMTimeConvertScale(pts, timescale: 1_000_000_000, method: .default).value
      ptsLats.append(Double(Int64(nowNs) - ptsNs) / 1e6)
    }
    lastSeq = s.seq; uniq.insert(s.seq)
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto p: AVCapturePhoto, error: Error?) { photoCB?(p, error) }
  func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo u: URL, from c: [AVCaptureConnection], error: Error?) { movieCB?(u, error) }
}

extension VC {
  func webcam() {
    let env = ProcessInfo.processInfo.environment
    let W = Int32(env["WC_W"] ?? "1920")!, H = Int32(env["WC_H"] ?? "1080")!, fps = Double(env["WC_FPS"] ?? "30")!, secs = Double(env["WC_SECS"] ?? "10")!
    let front = env["WC_POS"] == "front", movieSecs = Double(env["WC_MOVIE"] ?? "5")!, wantPhoto = env["WC_PHOTO"] != "0", wantPreview = env["WC_PREVIEW"] != "0"
    let pf = env["WC_FMT"] == "420v" ? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange : kCVPixelFormatType_32BGRA
    guard let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: front ? .front : .back) else { R(false, "wc.device"); done(); return }
    let m = WC(); wcKeep = m
    let s = session
    s.beginConfiguration()
    guard let inp = try? AVCaptureDeviceInput(device: dev), s.canAddInput(inp) else { R(false, "wc.input"); done(); return }
    s.addInput(inp)
    let vdo = AVCaptureVideoDataOutput(); vdo.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: pf]; vdo.alwaysDiscardsLateVideoFrames = true
    vdo.setSampleBufferDelegate(m, queue: DispatchQueue(label: "wc.vdo", qos: .userInteractive))
    if s.canAddOutput(vdo) { s.addOutput(vdo) }
    if let c = vdo.connection(with: .video) {
      if let r = env["WC_ROT"].flatMap(Double.init), c.isVideoRotationAngleSupported(CGFloat(r)) { c.videoRotationAngle = CGFloat(r) }
      if env["WC_MIRROR"] == "1", c.isVideoMirroringSupported { c.automaticallyAdjustsVideoMirroring = false; c.isVideoMirrored = true }
    }
    let po = AVCapturePhotoOutput(); if wantPhoto, s.canAddOutput(po) { s.addOutput(po) }
    let mo = AVCaptureMovieFileOutput(); if movieSecs > 0, s.canAddOutput(mo) { s.addOutput(mo) }
    s.commitConfiguration()
    if let f = dev.formats.first(where: { let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription); return d.width == W && d.height == H && $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= fps } && CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange }) ?? dev.formats.first(where: { let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription); return d.width == W && d.height == H }) {
      do { try dev.lockForConfiguration(); dev.activeFormat = f; dev.activeVideoMinFrameDuration = CMTime(value: 1, timescale: CMTimeScale(fps)); dev.activeVideoMaxFrameDuration = dev.activeVideoMinFrameDuration; dev.unlockForConfiguration()
        P("INFO wc.format \(fmtDesc(f)) fps=\(fps) pos=\(front ? "front" : "back")") } catch { R(false, "wc.format", "\(error)") }
    } else { R(false, "wc.format", "no \(W)x\(H)") }
    if wantPreview { let l = AVCaptureVideoPreviewLayer(session: s); l.frame = view.bounds; l.videoGravity = .resizeAspectFill; view.layer.addSublayer(l) }
    DispatchQueue.global().async {
      s.startRunning()
      P("INFO wc.running \(s.isRunning)")
      // WC_CYCLES > 1 (soak): the window, photo and movie again in the same session, each cycle's WCSTAT lines after a
      // WCCYCLE line; the session stops after the last one
      let cycles = max(1, Int(env["WC_CYCLES"] ?? "1") ?? 1)
      func measure(_ k: Int, after: Double) {
        DispatchQueue.global().asyncAfter(deadline: .now() + after) {   // the first second is warm-up, not counted
          if cycles > 1 { P("WCCYCLE \(k)") }
          m.lock.lock(); m.collecting = true; m.lastSeq = 0; m.frames = 0; m.fails = 0; m.dups = 0; m.gaps = 0; m.skipped = 0; m.uniq = []; m.lats = []; m.ptsLats = []; m.ptsBackwards = 0; m.bandFrames = 0
          m.firstT = 0; m.ptsGapsMs = []; m.t0 = CACurrentMediaTime(); m.sec = 0; m.sFrames = 0; m.sStamped = 0; m.sNew = 0; m.sBand = 0; m.counting = true; m.lock.unlock()
          DispatchQueue.global().asyncAfter(deadline: .now() + secs) {
            m.lock.lock(); m.flushSec(CACurrentMediaTime()); m.collecting = false
            let dur = m.lastT - m.firstT, fpsD = dur > 0 ? Double(m.frames - 1) / dur : 0, fpsU = dur > 0 ? Double(m.uniq.count) / dur : 0
            P(String(format: "WCSTAT vdo dims=%@ frames=%d secs=%.2f fps=%.2f uniqueFps=%.2f decodeFail=%d bandFrames=%d dupFrames=%d gaps=%d skippedSeqs=%d latMs p50=%.1f p95=%.1f max=%.1f min=%.1f ptsLatMs p50=%.1f p95=%.1f ptsBackwards=%d ptsGapMs p50=%.1f max=%.1f min=%.3f stampGeom=%d/%.4f",
                     m.dims, m.frames, dur, fpsD, fpsU, m.fails, m.bandFrames, m.dups, m.gaps, m.skipped, pct(m.lats, 0.5), pct(m.lats, 0.95), pct(m.lats, 1), pct(m.lats, 0),
                     pct(m.ptsLats, 0.5), pct(m.ptsLats, 0.95), m.ptsBackwards, pct(m.ptsGapsMs, 0.5), pct(m.ptsGapsMs, 1), pct(m.ptsGapsMs, 0), m.dec.lastT, m.dec.lastFrac))
            m.lock.unlock()
            self.wcPhoto(m, po, wantPhoto) { self.wcMovie(m, mo, movieSecs) { if k + 1 < cycles { measure(k + 1, after: 0) } else { s.stopRunning(); self.done() } } }
          }
        }
      }
      measure(0, after: 1.0)
    }
  }
  func wcPhoto(_ m: WC, _ po: AVCapturePhotoOutput, _ want: Bool, then: @escaping () -> Void) {
    guard want, !po.connections.isEmpty else { then(); return }
    let t0 = wcNow100us()
    m.photoCB = { p, e in
      let now = wcNow100us()
      if let p, let img = p.cgImageRepresentation(), let st = m.dec.decode(img) {
        P(String(format: "WCSTAT photo %dx%d seq=%u stampToPhotoMs=%.1f requestToPhotoMs=%.1f", img.width, img.height, st.seq, lat100(now, st.t), lat100(now, t0)))
        R(true, "wc.photo.stamp")
      } else { R(false, "wc.photo.stamp", "err=\(String(describing: e)) dims=\(p?.resolvedSettings.photoDimensions.width ?? 0)") }
      then()
    }
    po.capturePhoto(with: AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg]), delegate: m)
  }
  func wcMovie(_ m: WC, _ mo: AVCaptureMovieFileOutput, _ secs: Double, then: @escaping () -> Void) {
    guard secs > 0, !mo.connections.isEmpty else { then(); return }
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("wc-\(getpid()).mov"); try? FileManager.default.removeItem(at: url)
    m.movieCB = { u, e in
      DispatchQueue.global().async {
        let a = AVURLAsset(url: u)
        guard let tr = a.tracks(withMediaType: .video).first, let r = try? AVAssetReader(asset: a) else { R(false, "wc.movie", "no track err=\(String(describing: e))"); then(); return }
        let o = AVAssetReaderTrackOutput(track: tr, outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]); r.add(o); r.startReading()
        var n = 0, fails = 0, dups = 0, last: UInt32 = 0, uniq = Set<UInt32>(), first = CMTime.invalid, lastPts = CMTime.zero
        let d = StampDecoder()
        while let sb = o.copyNextSampleBuffer() {
          n += 1; let pts = CMSampleBufferGetPresentationTimeStamp(sb); if !first.isValid { first = pts }; lastPts = pts
          if let pb = CMSampleBufferGetImageBuffer(sb), let st = d.decode(pb) { if st.seq == last { dups += 1 }; last = st.seq; uniq.insert(st.seq) } else { fails += 1 }
        }
        let dur = CMTimeGetSeconds(lastPts - first), sz = tr.naturalSize
        P(String(format: "WCSTAT movie %.0fx%.0f nominalFps=%.1f frames=%d dur=%.2f decodeFail=%d dupFrames=%d uniqueSeqs=%d uniqueFps=%.2f err=%@ path=%@", sz.width, sz.height, tr.nominalFrameRate, n, dur, fails, dups, uniq.count, dur > 0 ? Double(uniq.count) / dur : 0, String(describing: e), u.path))
        R(n > 0 && fails == 0, "wc.movie.stamps")
        then()
      }
    }
    mo.startRecording(to: url, recordingDelegate: m)
    DispatchQueue.global().asyncAfter(deadline: .now() + secs) { mo.stopRecording() }
  }
}
var wcKeep: WC?
