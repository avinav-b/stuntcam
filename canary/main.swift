// SPDX-License-Identifier: Apache-2.0
// The canary: a throwaway app that drives the whole capture flow through AVFoundation's own consumers
// and exits with a verdict. Exit 0 = PASS, 1 = FAIL (checks), 4 = INCONCLUSIVE, 3 = watchdog. A crash = no RESULT line.
// Verdicts (design §5.2): core is PASS, FAIL(any content check) or INCONCLUSIVE (only timing checks failed: frame
// windows, fps, first-frame latency, photo/movie completion deadlines, the frame gap during a photo; or the overall
// budget ran out with no FAIL recorded). Late is timing, never is FAIL: no frame at all, a photo or movie that never
// completes, a photo completion without a photo. scripts/run.sh retries an INCONCLUSIVE once and counts a second one as
// FAIL, so a real hang still fails a test run but a starved machine does not; scripts/verify.sh (design §5.2) reports a
// second one as inconclusive(reason) and never caches it. Exit 3 (watchdog, nothing failed) is INCONCLUSIVE too.
// Stage retry: a photo stage (jpeg, 4x3, hvc1) whose only miss is the frame-gap check is run once more at the end, in a
// session of its own (attempt/runRetries); the RESULT line lists the first attempt in
// retried:[… -> retry ok|SLOW|FAIL]. Nothing else is retried in-process: start time (first frame), frame counts, a late
// or undelivered photo stay INCONCLUSIVE.
// Core flow: discovery, frames (VideoDataOutput + preview layer), QR (MetadataOutput), photo (JPEG + HEVC),
// reconfigure (activeFormat change + output swap while running), movie, capture + immediate stop, background/foreground,
// teardown + 2nd session.
// Extended (always run, reported separately): focus/exposure metering, torch, zoom readback, exposure bias completion +
// readback.
// Also core: didFinishCaptureFor, availableMetadataObjectTypes ⊇ {qr}, virtual devices (8-camera
// profile), front camera + mirroring; also ext: audio (mic input + AVCaptureAudioDataOutput). UIScene adopted (iOS 27
// SDK).
import UIKit
import AVFoundation
import ImageIO

let T0 = Date()
// 45 s: room for one "never" wait (<= 15 s) after a normal run (~15 s), so a never-completing photo/movie/frame is a
// FAIL within the budget, not INCONCLUSIVE. run.sh waits 60 s for the RESULT line.
let budget = Double(ProcessInfo.processInfo.environment["CANARY_BUDGET"] ?? "45") ?? 45
var stage = "init"
var coreFail: [String] = [], extFail: [String] = [], coreTiming: [String] = []
// Photo stages whose only miss was the frame-gap check, run once more at the end (see attempt()); each entry is
// "<first attempt's misses> -> retry ok|SLOW|FAIL"
var retried: [String] = []
let logLock = NSLock()
func log(_ s: String) {
  logLock.withLock {
    print(String(format: "CANARY| %6.3f ", Date().timeIntervalSince(T0)) + s)
    fflush(stdout)
  }
}
func check(_ c: Bool, timing: Bool = false, _ what: @autoclosure () -> String, ext: Bool = false) {
  let w = what()
  if !c {
    let e = "\(stage): \(w)"
    logLock.withLock {
      if ext { extFail.append(e) } else if timing { coreTiming.append(e) } else { coreFail.append(e) }
    }
  }
  log((c ? "ok   " : (timing ? "SLOW " : "FAIL ")) + (ext ? "[ext] " : "") + "\(stage): \(w)")
}
func finish() -> Never {
  let core = !coreFail.isEmpty ? "FAIL" : coreTiming.isEmpty ? "PASS" : "INCONCLUSIVE"
  let ext = extFail.isEmpty ? "PASS" : "FAIL"
  log("RESULT core=\(core) ext=\(ext) t=\(String(format: "%.1f", Date().timeIntervalSince(T0)))s"
      + retriedField()
      + (coreFail.isEmpty ? "" : " core:[\(coreFail.joined(separator: "; "))]")
      + (coreTiming.isEmpty ? "" : " timing:[\(coreTiming.joined(separator: "; "))]")
      + (extFail.isEmpty ? "" : " ext:[\(extFail.joined(separator: "; "))]"))
  exit(!coreFail.isEmpty ? 1 : coreTiming.isEmpty ? 0 : 4)
}
// before core:[ so the verdict parsers' core:[…] cut (scripts/verify.sh, the CLI's Classify) is unchanged
func retriedField() -> String {
  logLock.withLock { retried.isEmpty ? "" : " retried:[\(retried.joined(separator: "; "))]" }
}
func hostNow() -> Double { CMClockGetTime(CMClockGetHostTimeClock()).seconds }
func waitFor(_ secs: Double, _ cond: () -> Bool) -> Bool {
  let end = Date().addingTimeInterval(secs)
  while Date() < end {
    if cond() { return true }
    usleep(20_000)
  }
  return cond()
}
// a change to the device's configuration under its lock: lockForConfiguration (throws: body does not run), body,
// unlockForConfiguration
func withConfiguration(_ d: AVCaptureDevice, _ body: () -> Void) throws {
  try d.lockForConfiguration()
  body()
  d.unlockForConfiguration()
}
// a duration in seconds as the check messages print it: 4 -> "4", 5.5 -> "5.5"
func secondsText(_ s: Double) -> String { String(format: "%g", s) }
func onMain<T>(_ f: () -> T) -> T { Thread.isMainThread ? f() : DispatchQueue.main.sync(execute: f) }
// QR payload of the default source: the generated scene draws SIMCAM_QR_TEXT (default below); with SIMCAM_OFF_GENERATED_SCENE the
// fixture image carries M1B-QR-OK. CANARY_QR pins one value.
let qrDefaults = [ProcessInfo.processInfo.environment["SIMCAM_QR_TEXT"] ?? "https://simcam.dev/m12-qr", "M1B-QR-OK"]
let expectedQRs = ProcessInfo.processInfo.environment["CANARY_QR"].map { [$0] } ?? qrDefaults
let expectedQR = expectedQRs.joined(separator: "|")

struct FrameRec {
  let pts: Double; let arrival: Double; let w: Int; let h: Int; let fmt: OSType; let sig: UInt64; let nonzero: Bool
}

final class Canary: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureMetadataOutputObjectsDelegate,
  AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate, AVCaptureAudioDataOutputSampleBufferDelegate {
  let lock = NSLock()
  // objects a stage creates that must stay allocated until the process exits, though no later stage names them: freed
  // at teardown, their addresses could be reused by the second sessions' objects, which that stage is there to catch
  var keptUntilExit: [AnyObject] = []
  var frames: [FrameRec] = []
  var dropped = 0
  var qr: [(type: AVMetadataObject.ObjectType, str: String?, bounds: CGRect)] = []
  var events: [String] = []
  var photoEvents: [String] = []
  var photos: [(data: Data?, err: Error?, ts: Double, meta: [String: Any])] = []
  var photoDone = false
  // each capture()'s settings uniqueID and delivered timestamp, kept only for a capture with no FAIL and no timing miss
  // but the frame gap (a failed photo's ops may be missing), and its EXIF capture time (s since 1970; nil without the
  // SubsecTimeOriginal / OffsetTimeOriginal the engine writes)
  var tracePhotos: [(sid: Int64, ts: Double, exif: Double?)] = []
  var stopsAtFinish = -1   // didStopRunning notifications seen when the last didFinishCaptureFor arrived (photo-then-stop)
  var finishAt = Date.distantPast   // when the last didFinishCaptureFor arrived (photo-then-stop's stop timing)
  var movieDone: (URL, Error?)? = nil
  var bgSeen = false, fgSeen = false
  var window: UIWindow!
  var preview: AVCaptureVideoPreviewLayer?
  var obs: [NSKeyValueObservation] = []

  func ev(_ s: String) {
    lock.withLock { events.append(s) }
  }
  func snapshotFrames() -> [FrameRec] {
    lock.withLock { frames }
  }
  func resetFrames() {
    lock.withLock {
      frames = []
      dropped = 0
    }
  }

  // ---- delegates
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    let arrival = hostNow()
    if o is AVCaptureAudioDataOutput {
      lock.withLock {
        audioBufs += 1
        audioPTS.append(CMSampleBufferGetPresentationTimeStamp(sb).seconds)
        if let fd = CMSampleBufferGetFormatDescription(sb), let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd) {
          audioRate = asbd.pointee.mSampleRate
        }
      }
      return
    }
    if gridWant, let pb = CMSampleBufferGetImageBuffer(sb), CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA {
      let g = lumaGrid(pb)
      lock.withLock {
        grid = g
        gridWant = false
      }
    }
    guard let pb = CMSampleBufferGetImageBuffer(sb) else {
      lock.withLock { frames.append(FrameRec(pts: -1, arrival: arrival, w: 0, h: 0, fmt: 0, sig: 0, nonzero: false)) }
      return
    }
    CVPixelBufferLockBaseAddress(pb, .readOnly)
    var sig: UInt64 = 1469598103934665603
    var nz = false
    let base = CVPixelBufferIsPlanar(pb) ? CVPixelBufferGetBaseAddressOfPlane(pb, 0) : CVPixelBufferGetBaseAddress(pb)
    let bpr = CVPixelBufferIsPlanar(pb) ? CVPixelBufferGetBytesPerRowOfPlane(pb, 0) : CVPixelBufferGetBytesPerRow(pb)
    let h = CVPixelBufferIsPlanar(pb) ? CVPixelBufferGetHeightOfPlane(pb, 0) : CVPixelBufferGetHeight(pb)
    if let b = base?.assumingMemoryBound(to: UInt8.self) {
      for y in stride(from: 0, to: h, by: max(1, h / 256)) {
        for x in stride(from: 0, to: bpr, by: 4) {
          let v = b[y * bpr + x]
          if v != 0 { nz = true }
          sig = (sig ^ UInt64(v)) &* 1099511628211
        }
      }
    }
    CVPixelBufferUnlockBaseAddress(pb, .readOnly)
    let r = FrameRec(pts: CMSampleBufferGetPresentationTimeStamp(sb).seconds, arrival: arrival,
                     w: CVPixelBufferGetWidth(pb), h: CVPixelBufferGetHeight(pb),
                     fmt: CVPixelBufferGetPixelFormatType(pb), sig: sig, nonzero: nz)
    lock.withLock { frames.append(r) }
  }
  func captureOutput(_ o: AVCaptureOutput, didDrop sb: CMSampleBuffer, from c: AVCaptureConnection) {
    lock.withLock { dropped += 1 }
  }
  func metadataOutput(_ o: AVCaptureMetadataOutput, didOutput objs: [AVMetadataObject], from c: AVCaptureConnection) {
    lock.withLock {
      for m in objs { qr.append((m.type, (m as? AVMetadataMachineReadableCodeObject)?.stringValue, m.bounds)) }
    }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, willBeginCaptureFor r: AVCaptureResolvedPhotoSettings) {
    lock.withLock { photoEvents.append("willBegin \(r.photoDimensions.width)x\(r.photoDimensions.height)") }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, willCapturePhotoFor r: AVCaptureResolvedPhotoSettings) {
    lock.withLock { photoEvents.append("willCapture") }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didCapturePhotoFor r: AVCaptureResolvedPhotoSettings) {
    lock.withLock { photoEvents.append("didCapture") }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto p: AVCapturePhoto, error: Error?) {
    let d = p.fileDataRepresentation()
    lock.withLock {
      photoEvents.append("processed")
      photos.append((d, error, p.timestamp.seconds, p.metadata))
    }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishCaptureFor r: AVCaptureResolvedPhotoSettings, error: Error?) {
    lock.withLock {
      photoEvents.append("finished err=\(error.map { "\($0)" } ?? "nil")")
      photoDone = true
      stopsAtFinish = events.filter { $0.hasPrefix("didStop") }.count
      finishAt = Date()
    }
  }
  func fileOutput(_ o: AVCaptureFileOutput, didStartRecordingTo u: URL, from c: [AVCaptureConnection]) {
    ev("movieStarted")
  }
  func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo u: URL, from c: [AVCaptureConnection], error: Error?) {
    var e = error
    if let ns = error as NSError?, (ns.userInfo[AVErrorRecordingSuccessfullyFinishedKey] as? Bool) == true { e = nil }
    lock.withLock { movieDone = (u, e) }
  }

  // ---- checks
  // Time-to-first-frame is its own check with an explicit bound, and frame-rate windows start once frames flow. Fixed
  // windows right after startRunning failed on a cold first launch (first frame 1.8 s after process start on a virtual
  // runner, ~7 s right after a sim boot on the Mac) and after foregrounding, though frames then arrived normally.
  // CANARY_FIRST_FRAME_S overrides the bound (default 5 s).
  func firstFrame(_ label: String) {
    let limit = Double(ProcessInfo.processInfo.environment["CANARY_FIRST_FRAME_S"] ?? "") ?? 5.0
    let t0 = Date()
    resetFrames()
    let ok = waitFor(limit) { !self.snapshotFrames().isEmpty }
    check(ok, timing: true,
          "\(label) first frame within \(limit) s (took \(String(format: "%.2f", Date().timeIntervalSince(t0))) s)")
    if !ok, !waitFor(10, { !self.snapshotFrames().isEmpty }) {   // late is timing; never is a FAIL
      check(false, "\(label) no frame at all (\(limit + 10) s)")
    }
  }
  func checkFrames(_ label: String, expectW: Int? = nil, expectH: Int? = nil, minCount: Int = 12) {
    let fs = snapshotFrames()
    // none at all is a FAIL; too few is timing
    check(!fs.isEmpty, "\(label) any frame in the window (dropped=\(dropped))")
    if !fs.isEmpty {
      check(fs.count >= minCount, timing: true, "\(label) frames=\(fs.count) (>= \(minCount) in window) dropped=\(dropped)")
    }
    guard fs.count >= 2 else { return }
    check(fs.allSatisfy { $0.pts > 0 && $0.w > 0 }, "\(label) every frame has image buffer + valid pts")
    var mono = true
    for i in 1..<fs.count where fs[i].pts <= fs[i - 1].pts { mono = false }
    check(mono, "\(label) pts strictly increasing")
    let lag = fs.map { $0.arrival - $0.pts }
    check(lag.allSatisfy { $0 >= -0.05 && $0 < 0.5 },
          "\(label) pts on host clock (arrival-pts in [\(String(format: "%.3f", lag.min()!)), \(String(format: "%.3f", lag.max()!))])")
    check(fs.allSatisfy { $0.nonzero }, "\(label) pixels not all zero")
    check(Set(fs.map { $0.sig }).count > fs.count / 2,
          "\(label) content changes frame to frame (\(Set(fs.map { $0.sig }).count)/\(fs.count) distinct)")
    if let w = expectW, let h = expectH {
      check(fs.last!.w == w && fs.last!.h == h, "\(label) dims \(fs.last!.w)x\(fs.last!.h) == \(w)x\(h)")
    }
    let span = fs.last!.pts - fs.first!.pts
    if span > 0.3 {
      let fps = Double(fs.count - 1) / span
      check(fps >= 10, timing: true, "\(label) fps \(String(format: "%.1f", fps)) (>= 10)")
    }
  }

  // capture()'s deadlines: the photo is delivered within photoDeliverS and completes photoCompleteS after that; a late
  // one is drained for photoLateDrainS more before it counts as never completing
  static let photoDeliverS = 4.0, photoCompleteS = 1.5, photoLateDrainS = 8.0
  func capture(_ out: AVCapturePhotoOutput, codec: AVVideoCodecType, label: String? = nil) {
    stage = label ?? "photo-\(codec.rawValue)"
    let (cf0, ct0) = logLock.withLock { (coreFail.count, coreTiming.count) }
    lock.withLock {
      photoEvents = []
      photos = []
      photoDone = false
    }
    let s = AVCapturePhotoSettings(format: [AVVideoCodecKey: codec])
    s.maxPhotoDimensions = out.maxPhotoDimensions
    let t0 = hostNow()
    resetFrames()
    out.capturePhoto(with: s, delegate: self)
    let completeS = Canary.photoDeliverS + Canary.photoCompleteS, neverS = completeS + Canary.photoLateDrainS
    let arrived = waitFor(Canary.photoDeliverS) { lock.withLock { !photos.isEmpty || photoDone } }
    let ok = waitFor(Canary.photoCompleteS) { lock.withLock { photoDone } }
    if !ok {   // late: drain it, so its callbacks can't land in the next stage (seen on a starved hosted runner)
      let tl = Date()
      let late = waitFor(Canary.photoLateDrainS) { lock.withLock { photoDone } }
      check(false, timing: true,
            "photo completed within \(secondsText(completeS)) s ("
              + (late ? String(format: "completed %.1f s later", Date().timeIntervalSince(tl)) : "not after \(secondsText(neverS)) s")
              + ")")
      if !late {   // late but completed: the content checks below still run
        check(false, "photo never completed (\(secondsText(neverS)) s)")
        return
      }
    }
    let t1 = hostNow()
    if !arrived { check(false, timing: true, "photo delivered within \(secondsText(Canary.photoDeliverS)) s") }
    // Encoding on the frame pump stalls preview/VDO frames for 125-283 ms per photo (2 s in a burst); on its own queue
    // the longest gap is ~35 ms. Over 100 ms (3 frame intervals at 30 fps) means the pump is stalled again.
    let arr = snapshotFrames().map { $0.arrival }.filter { $0 >= t0 && $0 <= t1 }
    var gap = 0.0, prev = t0
    for a in arr {
      gap = max(gap, a - prev)
      prev = a
    }
    gap = max(gap, t1 - prev)
    check(gap <= 0.1, timing: true, "longest frame gap during the photo \(Int(gap * 1000)) ms (<= 100 ms, \(arr.count) frames)")
    let (evs, ph) = lock.withLock { (photoEvents, photos) }
    let order = evs.map { $0.split(separator: " ").first.map(String.init) ?? $0 }
    check(order.starts(with: ["willBegin", "willCapture", "didCapture", "processed"]), "callback order \(order)")
    guard let p = ph.first else {
      check(false, "AVCapturePhoto delivered before didFinishCaptureFor (events \(evs))")
      return
    }
    check(p.err == nil, "photo error \(String(describing: p.err))")
    check(p.ts >= t0 - 0.5 && p.ts <= t1 + 0.05,
          "photo.timestamp on host clock (\(String(format: "%.3f", p.ts)) in [\(String(format: "%.3f", t0)), \(String(format: "%.3f", t1))])")
    guard let d = p.data, let src = CGImageSourceCreateWithData(d as CFData, nil),
          let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [String: Any] else {
      check(false, "fileDataRepresentation decodes (bytes=\(p.data?.count ?? -1))")
      return
    }
    let type = CGImageSourceGetType(src) as String? ?? "?"
    let w = props[kCGImagePropertyPixelWidth as String] as? Int ?? 0, h = props[kCGImagePropertyPixelHeight as String] as? Int ?? 0
    let want = codec == .jpeg ? "public.jpeg" : "public.heic"
    check(type == want, "container \(type) == \(want)")
    let md = out.maxPhotoDimensions
    check(max(w, h) == Int(max(md.width, md.height)) && min(w, h) == Int(min(md.width, md.height)),
          "pixels \(w)x\(h) == maxPhotoDimensions \(md.width)x\(md.height)")
    let exif = props[kCGImagePropertyExifDictionary as String] as? [String: Any]
    check(props[kCGImagePropertyOrientation as String] != nil && exif?[kCGImagePropertyExifDateTimeOriginal as String] != nil,
          "EXIF Orientation + DateTimeOriginal present")
    check(p.meta[kCGImagePropertyOrientation as String] != nil, "AVCapturePhoto.metadata has Orientation")
    let clean = logLock.withLock { coreFail.count == cf0 && coreTiming[ct0...].allSatisfy { $0.contains(Canary.gapCheck) } }
    if clean { tracePhotos.append((s.uniqueID, p.ts, exifTime(exif))) }
  }
  // EXIF DateTimeOriginal + SubsecTimeOriginal + OffsetTimeOriginal as seconds since 1970 (nil if one is missing)
  func exifTime(_ exif: [String: Any]?) -> Double? {
    guard let e = exif, let dt = e[kCGImagePropertyExifDateTimeOriginal as String] as? String,
          let sub = e[kCGImagePropertyExifSubsecTimeOriginal as String] as? String,
          let ofs = e[kCGImagePropertyExifOffsetTimeOriginal as String] as? String else { return nil }
    let f = DateFormatter()
    f.locale = Locale(identifier: "en_US_POSIX")
    f.dateFormat = "yyyy:MM:dd HH:mm:ssxxx"
    guard let d = f.date(from: dt + ofs), let frac = Double("0." + sub) else { return nil }
    return d.timeIntervalSince1970 + frac
  }

  // Front camera, portrait VDO connection, mirroring on vs off must flip the picture horizontally
  var gridWant = false
  var grid: [Float]? = nil
  var gridDims = (0, 0)
  func lumaGrid(_ pb: CVPixelBuffer) -> [Float] {
    CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
    let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb), bpr = CVPixelBufferGetBytesPerRow(pb)
    guard let b = CVPixelBufferGetBaseAddress(pb)?.assumingMemoryBound(to: UInt8.self) else { return [] }
    var g = [Float](repeating: 0, count: 32 * 32)
    for gy in 0..<32 {
      for gx in 0..<32 {
        let x = (gx * w / 32 + w / 64), y = (gy * h / 32 + h / 64)
        let o = y * bpr + x * 4
        g[gy * 32 + gx] = Float(b[o]) + Float(b[o + 1]) + Float(b[o + 2])
      }
    }
    return g
  }
  func frontMirror() {
    stage = "front-mirror"
    guard let fd = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) else {
      check(false, "front camera present")
      return
    }
    let s = AVCaptureSession()
    s.beginConfiguration()
    guard let fi = try? AVCaptureDeviceInput(device: fd), s.canAddInput(fi) else {
      check(false, "front input")
      return
    }
    s.addInput(fi)
    let v = AVCaptureVideoDataOutput()
    v.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
    v.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.front"))
    s.addOutput(v)
    guard let c = v.connection(with: .video) else {
      check(false, "front vdo connection")
      return
    }
    if c.isVideoRotationAngleSupported(90) { c.videoRotationAngle = 90 }
    check(c.isVideoMirroringSupported, "front connection supports mirroring")
    c.automaticallyAdjustsVideoMirroring = false
    c.isVideoMirrored = false
    s.commitConfiguration()
    s.startRunning()
    func takeGrid() -> [Float]? {
      lock.withLock {
        grid = nil
        gridWant = true
      }
      _ = waitFor(1.5) { lock.withLock { grid != nil } }
      return lock.withLock { grid }
    }
    firstFrame("front")
    resetFrames()
    usleep(400_000)
    checkFrames("front", minCount: 6)
    let f = snapshotFrames().last
    check(f.map { $0.h > $0.w } ?? false, "front portrait buffer \(f.map { "\($0.w)x\($0.h)" } ?? "-")")
    let a = takeGrid()
    s.beginConfiguration()
    c.isVideoMirrored = true
    s.commitConfiguration()
    usleep(250_000)
    let b = takeGrid()
    s.stopRunning()
    guard let A = a, let B = b, A.count == 1024, B.count == 1024 else {
      check(false, "front grids captured")
      return
    }
    var dSame: Float = 0, dFlip: Float = 0
    for y in 0..<32 {
      for x in 0..<32 {
        dSame += abs(B[y * 32 + x] - A[y * 32 + x])
        dFlip += abs(B[y * 32 + x] - A[y * 32 + 31 - x])
      }
    }
    check(dFlip < dSame * 0.5,
          "mirrored frame = horizontal flip of unmirrored (diff flip \(Int(dFlip)) vs same \(Int(dSame)))")
  }
  // ext: microphone input + AVCaptureAudioDataOutput deliver buffers on the host clock
  var audioBufs = 0
  var audioPTS: [Double] = []
  var audioRate = 0.0
  func audio() {
    stage = "audio"
    guard let mic = AVCaptureDevice.default(for: .audio) else {
      check(false, "microphone device present", ext: true)
      return
    }
    let s = AVCaptureSession()
    s.beginConfiguration()
    guard let mi = try? AVCaptureDeviceInput(device: mic), s.canAddInput(mi) else {
      check(false, "mic input", ext: true)
      return
    }
    s.addInput(mi)
    let ado = AVCaptureAudioDataOutput()
    ado.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.ado"))
    s.addOutput(ado)
    s.commitConfiguration()
    s.startRunning()
    usleep(1_000_000)
    s.stopRunning()
    let (n, pts, rate) = lock.withLock { (audioBufs, audioPTS, audioRate) }
    check(n >= 20, "audio buffers in 1 s: \(n) rate \(rate)", ext: true)
    let now = hostNow()
    check(!pts.isEmpty && pts.allSatisfy { now - $0 < 3 && now - $0 > 0 }, "audio pts on host clock", ext: true)
  }

  // ext (ledger L67): the engine's op ring (`stuntcam trace`, engine/src/core/trace.inc) has each photo taken above,
  // its stages in order with non-decreasing host times, and the timestamp op equal to the AVCapturePhoto.timestamp
  // delivered. The file is trace-<pid>.txt in $SIMCAM_STATE_DIR/<bundle>/, else <container tmp>/simcam-state/<bundle>/
  // (written ~150 ms after an op).
  func traceCheck() {
    stage = "trace"
    let want = tracePhotos
    guard !want.isEmpty else {
      check(false, "photos to look for in the trace", ext: true)
      return
    }
    let b = Bundle.main.bundleIdentifier ?? "-", name = "trace-\(getpid()).txt"
    var dirs: [String] = []
    if let sd = ProcessInfo.processInfo.environment["SIMCAM_STATE_DIR"], !sd.isEmpty { dirs.append("\(sd)/\(b)") }
    dirs.append((NSTemporaryDirectory() as NSString).appendingPathComponent("simcam-state/\(b)"))
    let stages = ["shutter", "slot12", "will-capture", "timestamp", "did-capture", "encode-start", "encoded",
                  "still-complete", "did-finish"]
    var path = "", ops: [(name: String, mach: UInt64, sid: Int64, arg: Int64, wall: UInt64)] = []
    func have() -> Bool {   // every photo's did-finish (the last stage) is in the file
      for d in dirs {
        let p = "\(d)/\(name)"
        guard let txt = try? String(contentsOfFile: p, encoding: .utf8), txt.hasSuffix("end\n") else { continue }
        path = p
        ops = []
        for l in txt.split(separator: "\n") {
          let f = l.split(separator: " ")
          guard f.count == 7, f[0] == "op", let wl = UInt64(f[2]), let m = UInt64(f[3]), let s = Int64(f[5]),
                let a = Int64(f[6]) else { continue }
          ops.append((String(f[4]), m, s, a, wl))
        }
        return want.allSatisfy { w in ops.contains { $0.sid == w.sid && $0.name == "did-finish" } }
      }
      return false
    }
    let found = waitFor(2) { have() }
    check(!path.isEmpty, "trace file \(name) in \(dirs)", ext: true)
    guard !path.isEmpty else { return }
    check(found, "trace has did-finish for photos \(want.map { $0.sid }) (\(ops.count) ops)", ext: true)
    var tb = mach_timebase_info_data_t()
    mach_timebase_info(&tb)
    for w in want {
      let mine = ops.filter { $0.sid == w.sid && stages.contains($0.name) }
      let names = mine.map { $0.name }
      check(names == stages, "photo \(w.sid) stages in order \(names)", ext: true)
      check(zip(mine, mine.dropFirst()).allSatisfy { $0.mach <= $1.mach }, "photo \(w.sid) host times non-decreasing",
            ext: true)
      if let t = mine.first(where: { $0.name == "timestamp" }) {
        let viaMach = Double(t.mach) * Double(tb.numer) / Double(tb.denom) / 1e9
        check(abs(Double(t.arg) / 1e9 - w.ts) < 1e-6 && abs(viaMach - w.ts) < 1e-6,
              "photo \(w.sid) timestamp op \(t.arg) ns == AVCapturePhoto.timestamp \(String(format: "%.9f", w.ts))",
              ext: true)
        // ledger L68 (design §9): the timestamp is the capture moment, within 50 ms of the slot-12 mark
        if let s12 = mine.first(where: { $0.name == "slot12" }) {
          let d = (Double(t.mach) - Double(s12.mach)) * Double(tb.numer) / Double(tb.denom) / 1e6
          check(d >= 0 && d <= 50, "photo \(w.sid) timestamp - slot12 \(String(format: "%.1f", d)) ms (0..50 ms)",
                ext: true)
          // ledger L72: the EXIF capture time is the timestamp's instant (the frame grab) on the wall clock, which the
          // timestamp op carries: equal to its ms (DateTimeOriginal + SubsecTimeOriginal + OffsetTimeOriginal, rounded
          // to the ms; one wall value, back-computed from the grab's mach time, feeds both). Not against slot 12 with
          // the 50 ms window: on 27.0 the whole encode takes 10-18 ms, so an encode-time stamp would fit it too
          if let x = w.exif {
            let dx = (x - Double(t.wall) / 1e9) * 1000, d12 = (x - Double(s12.wall) / 1e9) * 1000
            check(dx >= -2 && dx <= 2,
                  "photo \(w.sid) EXIF DateTimeOriginal - timestamp \(String(format: "%.1f", dx)) ms"
                    + " (-2..2 ms; - slot12 \(String(format: "%.1f", d12)) ms)",
                  ext: true)
          } else {
            check(false, "photo \(w.sid) EXIF DateTimeOriginal with SubsecTimeOriginal + OffsetTimeOriginal", ext: true)
          }
        }
      }
    }
  }

  func previewHasContents() -> Bool {
    onMain {
      func walk(_ l: CALayer) -> Bool {
        if l.contents != nil { return true }
        return (l.sublayers ?? []).contains(where: walk)
      }
      return preview.map(walk) ?? false
    }
  }

  // ---- stage retry
  // A photo stage whose only miss is the frame-gap check is run once more, after every other stage, in a session of
  // its own, with the same threshold. Right after a boot the frame pump can stall (27.0's HEVC photo: 129-158 ms on
  // 3/37 first launches; 116-886 ms when the canary starts ~2 s after bootstatus). A retry in the same, already warm
  // process can't tell that stall from a regression that only hits a process's first photo (lazy encoder setup on the
  // pump), so it is limited to the gap check, the CLI reports a retried pass as such and caches it only briefly, and on
  // 27.x the CLI waits out the post-boot window before the launch (settle floor). A retry that misses again leaves the
  // run INCONCLUSIVE; a content FAIL in the retry is a FAIL.
  static let gapCheck = "longest frame gap during the photo"
  var retryLater: [(stage: String, first: [String], run: () -> Void)] = []
  func attempt(_ name: String, retry: @escaping () -> Void, _ body: () -> Void) {
    let (f0, t0) = logLock.withLock { (coreFail.count, coreTiming.count) }
    body()
    let (misses, gapOnly) = logLock.withLock {
      let failed = coreFail.count > f0, misses = Array(coreTiming[t0...])
      let gapOnly = !failed && !misses.isEmpty && misses.allSatisfy { $0.contains(Canary.gapCheck) }
      if gapOnly { coreTiming.removeSubrange(t0...) }
      return (misses, gapOnly)
    }
    if gapOnly {
      log("RETRY-LATER \(name): \(misses.joined(separator: "; "))")
      retryLater.append((name, misses, retry))
    }
  }
  func runRetries() {
    for r in retryLater {
      let (f0, t0) = logLock.withLock { (coreFail.count, coreTiming.count) }
      log("RETRY \(r.stage)")
      r.run()
      let outcome = logLock.withLock {
        let outcome = coreFail.count > f0 ? "FAIL" : coreTiming.count > t0 ? "SLOW" : "ok"
        retried.append("\(r.first.joined(separator: "; ")) -> retry \(outcome)")
        return outcome
      }
      log("RETRY \(r.stage) -> \(outcome)")
    }
  }
  // a retried photo, in a session of its own: the back camera on format f, a VideoDataOutput (frames for the gap check)
  // and a photo output with the first attempt's codec and photo dimensions
  func retryPhoto(_ dev: AVCaptureDevice, _ f: AVCaptureDevice.Format?, codec: AVVideoCodecType, label: String,
                  dims: CMVideoDimensions?) {
    let s = AVCaptureSession()
    s.beginConfiguration()
    s.sessionPreset = .inputPriority
    stage = label
    guard let i = try? AVCaptureDeviceInput(device: dev), s.canAddInput(i) else {
      s.commitConfiguration()
      check(false, "retry session input")
      return
    }
    s.addInput(i)
    let v = AVCaptureVideoDataOutput()
    v.alwaysDiscardsLateVideoFrames = true
    v.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.retry"))
    s.addOutput(v)
    let p = AVCapturePhotoOutput()
    s.addOutput(p)
    if let f = f {
      do {
        try withConfiguration(dev) { dev.activeFormat = f }
      } catch { check(false, "retry lock \(error)") }
    }
    s.commitConfiguration()
    s.startRunning()
    firstFrame("\(label) retry")
    usleep(300_000)
    if let d = dims {
      p.maxPhotoDimensions = d
    } else if let m = dev.activeFormat.supportedMaxPhotoDimensions.max(by: { $0.width * $0.height < $1.width * $1.height }) {
      p.maxPhotoDimensions = m
    }
    capture(p, codec: codec, label: label)
    s.stopRunning()
    for o in s.outputs { s.removeOutput(o) }
    for i in s.inputs { s.removeInput(i) }
    usleep(150_000)
  }

  // ---- the run: one function per stage, in this order (each sets its stage name for the check lines)
  // the first session (VideoDataOutput + metadata + photo + preview), built by configureSession and used by later stages
  struct MainSession {
    let session: AVCaptureSession
    let vdo: AVCaptureVideoDataOutput
    let meta: AVCaptureMetadataOutput
    let photo: AVCapturePhotoOutput
    let f1080: AVCaptureDevice.Format?   // the 16:9 1920x1080 format, if present: the stages switch back to it
  }
  func run() {
    observeNotifications()
    let dev = discoverDevices()   // 1. discovery
    checkVirtualDevices()
    let a = configureSession(dev)   // 2. session A: VDO + metadata + photo + preview
    startSession(a)
    let d0 = checkFirstFrames(dev)   // 3. frames through AVFoundation's own consumer
    checkQR()   // 4. QR
    capturePhotos(dev, a)   // 5. photo
    traceCheck()
    checkDeviceControls(dev)   // 5b. extended device controls
    reconfigureFormat(dev, d0, a.f1080)   // 6. reconfigure while running: change activeFormat, then swap outputs
    let movie = reconfigureOutputs(a)
    recordMovie(dev, movie)   // 7. movie
    photoThenStop(a)   // 7b
    backgroundForeground(a, movie)   // 8. background / foreground
    teardown(a)   // 9. teardown + a second session
    secondSessions(dev)
    usleep(200_000)
    frontMirror()
    audio()
    let lastStage = stage
    runRetries()
    stage = lastStage   // the overall check keeps its stage name
    check(evCount("runtimeError") == 0, "no AVCaptureSessionRuntimeError overall (\(evCount("runtimeError")))")
    finish()
  }
  func evCount(_ s: String) -> Int {
    lock.withLock { events.filter { $0.hasPrefix(s) }.count }
  }
  func observeNotifications() {
    let nc = NotificationCenter.default
    nc.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: nil, queue: nil) { n in
      self.ev("runtimeError \(n.userInfo?[AVCaptureSessionErrorKey] ?? "")")
      log("runtimeError \(n.userInfo?[AVCaptureSessionErrorKey] ?? "")")
    }
    nc.addObserver(forName: AVCaptureSession.didStartRunningNotification, object: nil, queue: nil) { _ in self.ev("didStart") }
    nc.addObserver(forName: AVCaptureSession.didStopRunningNotification, object: nil, queue: nil) { _ in self.ev("didStop") }
    nc.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: nil, queue: nil) { _ in
      self.ev("interrupted")
    }
    nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil) { _ in
      self.bgSeen = true
      log("app did enter background")
    }
    nc.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { _ in
      self.fgSeen = true
      log("app will enter foreground")
    }
  }
  func discoverDevices() -> AVCaptureDevice {
    stage = "discovery"
    let ds = AVCaptureDevice.DiscoverySession(
      deviceTypes: [.builtInWideAngleCamera, .builtInTripleCamera, .builtInDualWideCamera, .builtInTrueDepthCamera],
      mediaType: .video, position: .unspecified)
    keptUntilExit.append(ds)
    let devs = ds.devices
    check(!devs.isEmpty, "DiscoverySession devices=\(devs.map { "\($0.localizedName)/\($0.position.rawValue)" })")
    guard let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else {
      check(false, "default back wide")
      finish()
    }
    check(NSStringFromClass(type(of: dev)).contains("FigVideoDevice"), "genuine device class \(type(of: dev))")
    check(AVCaptureDevice(uniqueID: dev.uniqueID) === dev, "device(uniqueID:) identity")
    check(dev.formats.count >= 2, "formats=\(dev.formats.count)")
    check(dev.formats.allSatisfy { !$0.videoSupportedFrameRateRanges.isEmpty }, "every format has fps ranges")
    return dev
  }
  func checkVirtualDevices() {
    // Virtual devices of the 8-camera profile (only when the profile has them)
    stage = "virtual-devices"
    let all = AVCaptureDevice.DiscoverySession(
      deviceTypes: [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera, .builtInDualWideCamera,
                    .builtInTripleCamera, .builtInTrueDepthCamera, .builtInLiDARDepthCamera],
      mediaType: .video, position: .unspecified).devices
    log("devices: \(all.map { "\($0.localizedName)" })")
    if let tri = all.first(where: { $0.deviceType == .builtInTripleCamera }) {
      let c = tri.constituentDevices.map { $0.deviceType }
      check(tri.isVirtualDevice && c == [.builtInUltraWideCamera, .builtInWideAngleCamera, .builtInTelephotoCamera],
            "Triple virtual, constituents \(c.map { $0.rawValue })")
      check(tri.virtualDeviceSwitchOverVideoZoomFactors.map { $0.doubleValue } == [2, 8],
            "Triple switch-over \(tri.virtualDeviceSwitchOverVideoZoomFactors)")
      let dw = all.first { $0.deviceType == .builtInDualWideCamera }
      check(dw?.constituentDevices.map { $0.deviceType } == [.builtInUltraWideCamera, .builtInWideAngleCamera],
            "DualWide constituents")
      check(all.count == 8, "8 cameras listed (\(all.count))")
    } else {
      log("profile without virtual devices: skipped")
    }
  }
  func configureSession(_ dev: AVCaptureDevice) -> MainSession {
    stage = "configure"
    let session = AVCaptureSession()
    session.beginConfiguration()
    session.sessionPreset = .inputPriority
    guard let input = try? AVCaptureDeviceInput(device: dev), session.canAddInput(input) else {
      check(false, "add input")
      finish()
    }
    keptUntilExit.append(input)
    session.addInput(input)
    let vdo = AVCaptureVideoDataOutput()
    vdo.alwaysDiscardsLateVideoFrames = true
    vdo.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.vdo"))
    check(session.canAddOutput(vdo), "canAddOutput(VideoData)")
    session.addOutput(vdo)
    let meta = AVCaptureMetadataOutput()
    check(session.canAddOutput(meta), "canAddOutput(Metadata)")
    session.addOutput(meta)
    meta.setMetadataObjectsDelegate(self, queue: DispatchQueue(label: "canary.meta"))
    let photo = AVCapturePhotoOutput()
    check(session.canAddOutput(photo), "canAddOutput(Photo)")
    session.addOutput(photo)
    // pick a 16:9 1920x1080 format if present
    let f1080 = dev.formats.first {
      let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription)
      return d.width == 1920 && d.height == 1080 && $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 30 }
    }
    do {
      try withConfiguration(dev) { if let f = f1080 { dev.activeFormat = f } }
    } catch { check(false, "lockForConfiguration \(error)") }
    if let maxd = dev.activeFormat.supportedMaxPhotoDimensions.max(by: { $0.width * $0.height < $1.width * $1.height }) {
      photo.maxPhotoDimensions = maxd
    }
    session.commitConfiguration()
    onMain {
      let pl = AVCaptureVideoPreviewLayer(session: session)
      pl.frame = window.bounds
      pl.videoGravity = .resizeAspectFill
      window.rootViewController!.view.layer.addSublayer(pl)
      preview = pl
    }
    return MainSession(session: session, vdo: vdo, meta: meta, photo: photo, f1080: f1080)
  }
  func startSession(_ a: MainSession) {
    let session = a.session, meta = a.meta
    stage = "start"
    session.startRunning()
    check(waitFor(2) { evCount("didStart") >= 1 }, "didStartRunning notification")
    check(session.isRunning, "isRunning")
    // like expo-camera: only enable the types the output says are available
    check(waitFor(1) { meta.availableMetadataObjectTypes.contains(.qr) },
          "availableMetadataObjectTypes contains qr (\(meta.availableMetadataObjectTypes.map { $0.rawValue }))")
    if meta.availableMetadataObjectTypes.contains(.qr) { meta.metadataObjectTypes = [.qr] }
  }
  // returns the format's dimensions the frames were checked against
  func checkFirstFrames(_ dev: AVCaptureDevice) -> CMVideoDimensions {
    stage = "frames"
    let d0 = CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription)
    firstFrame("A")   // the cold start time (design §5.3): never retried in-process
    resetFrames()
    usleep(1_000_000)
    checkFrames("A", expectW: Int(d0.width), expectH: Int(d0.height))
    stage = "preview"
    check(waitFor(1) { previewHasContents() }, "preview layer (or a sublayer) has contents")
    return d0
  }
  func checkQR() {
    stage = "qr"
    let gotQR = waitFor(2.5) { lock.withLock { qr.contains { $0.type == .qr && expectedQRs.contains($0.str ?? "") } } }
    let q = lock.withLock { qr.prefix(3).map { "\($0.type.rawValue):\($0.str ?? "nil"):\($0.bounds)" } }
    check(gotQR, "QR '\(expectedQR)' via AVMetadataMachineReadableCodeObject (seen \(q))")
  }
  func capturePhotos(_ dev: AVCaptureDevice, _ a: MainSession) {
    let photo = a.photo, f1080 = a.f1080
    attempt("photo-jpeg", retry: { [self] in retryPhoto(dev, f1080, codec: .jpeg, label: "photo-jpeg", dims: nil) }) {
      capture(photo, codec: .jpeg)
    }
    if let f43 = dev.formats.first(where: {
         $0.supportedMaxPhotoDimensions.contains { $0.width * 3 == $0.height * 4 && $0.width >= 4000 }
       }),
       let md = f43.supportedMaxPhotoDimensions.filter({ $0.width * 3 == $0.height * 4 }).max(by: { $0.width < $1.width }) {
      // ledger L06: a 4:3 format at its full size; on the 16:9 format the engine's fallback size equals the max, so a
      // missing photo-dims fix went unnoticed in a mutation run
      let prevMax = photo.maxPhotoDimensions   // read before the switch: the 4:3 activeFormat resets it (restoring that value throws)
      do {
        try withConfiguration(dev) { dev.activeFormat = f43 }
      } catch { check(false, "lock \(error)") }
      usleep(300_000)
      photo.maxPhotoDimensions = md
      attempt("photo-4x3", retry: { [self] in retryPhoto(dev, f43, codec: .jpeg, label: "photo-4x3", dims: md) }) {
        capture(photo, codec: .jpeg, label: "photo-4x3")
      }
      try? withConfiguration(dev) { if let f = f1080 { dev.activeFormat = f } }
      photo.maxPhotoDimensions = prevMax   // the 16:9 format does not support 4:3 sizes; the HEVC capture below would throw
      usleep(300_000)
    } else {
      stage = "photo-4x3"
      check(false, "a format with a 4:3 photo size >= 4000 wide")
    }
    if photo.availablePhotoCodecTypes.contains(.hevc) {
      attempt("photo-hvc1", retry: { [self] in retryPhoto(dev, f1080, codec: .hevc, label: "photo-hvc1", dims: nil) }) {
        capture(photo, codec: .hevc)
      }
    } else {
      stage = "photo-hvc1"
      check(false, "HEVC available (\(photo.availablePhotoCodecTypes))")
    }
  }
  func checkDeviceControls(_ dev: AVCaptureDevice) {
    stage = "ext-controls"
    do {
      var adjF = [Bool](), adjE = [Bool](), torch = [Bool]()
      obs.append(dev.observe(\.isAdjustingFocus, options: [.new]) { _, c in
        self.lock.withLock { adjF.append(c.newValue ?? false) }
      })
      obs.append(dev.observe(\.isAdjustingExposure, options: [.new]) { _, c in
        self.lock.withLock { adjE.append(c.newValue ?? false) }
      })
      obs.append(dev.observe(\.isTorchActive, options: [.new]) { _, c in
        self.lock.withLock { torch.append(c.newValue ?? false) }
      })
      try withConfiguration(dev) {
        if dev.isFocusPointOfInterestSupported {
          dev.focusPointOfInterest = CGPoint(x: 0.3, y: 0.3)
          dev.focusMode = .autoFocus
        }
        if dev.isExposurePointOfInterestSupported {
          dev.exposurePointOfInterest = CGPoint(x: 0.3, y: 0.3)
          dev.exposureMode = .autoExpose
        }
      }
      check(waitFor(1.5) { lock.withLock { adjF.contains(true) && adjF.last == false } },
            "focus metering cycles isAdjustingFocus YES->NO (\(adjF))", ext: true)
      check(waitFor(0.5) { lock.withLock { adjE.contains(true) && adjE.last == false } },
            "exposure metering cycles YES->NO (\(adjE))", ext: true)
      if dev.hasTorch {
        try withConfiguration(dev) { dev.torchMode = .on }
        check(waitFor(1) { dev.isTorchActive }, "torch on -> isTorchActive (\(torch))", ext: true)
        try withConfiguration(dev) { dev.torchMode = .off }
        check(waitFor(1) { !dev.isTorchActive }, "torch off -> !isTorchActive", ext: true)
      }
      try withConfiguration(dev) { dev.videoZoomFactor = min(2, dev.activeFormat.videoMaxZoomFactor) }
      check(abs(dev.videoZoomFactor - min(2, dev.activeFormat.videoMaxZoomFactor)) < 0.01,
            "zoom readback \(dev.videoZoomFactor)", ext: true)
      try withConfiguration(dev) { dev.videoZoomFactor = 1 }
      // ledger L03: exposure bias must complete and read back (a renamed notification key once left the bias promise
      // hanging in a VisionCamera app while every other canary check passed)
      let want = min(max(1.0, dev.minExposureTargetBias), dev.maxExposureTargetBias)
      var biasDone = false
      try withConfiguration(dev) {
        dev.setExposureTargetBias(want) { _ in
          self.lock.withLock { biasDone = true }
        }
      }
      check(waitFor(1) { lock.withLock { biasDone } },
            "exposure bias completion handler within 1 s", ext: true)
      check(abs(dev.exposureTargetBias - want) < 0.01, "exposure bias readback \(dev.exposureTargetBias) == \(want)",
            ext: true)
      try withConfiguration(dev) { dev.setExposureTargetBias(0, completionHandler: nil) }
    } catch { check(false, "device controls threw \(error)", ext: true) }
  }
  func reconfigureFormat(_ dev: AVCaptureDevice, _ d0: CMVideoDimensions, _ f1080: AVCaptureDevice.Format?) {
    stage = "reconfigure-format"
    let other = dev.formats.first {
      let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription)
      return d.width != d0.width || d.height != d0.height
    }
    if let f = other {
      do {
        try withConfiguration(dev) { dev.activeFormat = f }
      } catch { check(false, "lock \(error)") }
      usleep(400_000)
      firstFrame("B")
      resetFrames()
      usleep(800_000)
      let d1 = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
      checkFrames("B", expectW: Int(d1.width), expectH: Int(d1.height), minCount: 10)
      try? withConfiguration(dev) { if let f = f1080 { dev.activeFormat = f } }
    } else {
      check(false, "no second format to switch to")
    }
  }
  func reconfigureOutputs(_ a: MainSession) -> AVCaptureMovieFileOutput {
    let session = a.session, vdo = a.vdo, meta = a.meta
    stage = "reconfigure-outputs"
    let movie = AVCaptureMovieFileOutput()
    session.beginConfiguration()
    session.removeOutput(vdo)
    session.removeOutput(meta)
    check(session.canAddOutput(movie), "canAddOutput(MovieFile) after removing VDO")
    session.addOutput(movie)
    session.commitConfiguration()
    check(session.isRunning && evCount("runtimeError") == 0, "still running, no runtime error after reconfigure")
    check(movie.connection(with: .video)?.isActive == true, "movie connection active")
    return movie
  }
  func recordMovie(_ dev: AVCaptureDevice, _ movie: AVCaptureMovieFileOutput) {
    stage = "movie"
    // ledger L16: record on a non-1080p format and compare the track with the active format; 1080p is also the engine's
    // fallback size, so a recording that ignored the active format passed a mutation run
    let fMovie = dev.formats.filter {
      let d = CMVideoFormatDescriptionGetDimensions($0.formatDescription)
      return !(d.width == 1920 && d.height == 1080) && $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 30 }
    }
      .min {
        let a = CMVideoFormatDescriptionGetDimensions($0.formatDescription),
            b = CMVideoFormatDescriptionGetDimensions($1.formatDescription)
        return Int(a.width) * Int(a.height) < Int(b.width) * Int(b.height)
      }
    if let f = fMovie {
      try? withConfiguration(dev) { dev.activeFormat = f }
      usleep(300_000)
    }
    let dm = CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription)
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("canary-\(Int(Date().timeIntervalSince1970)).mov")
    try? FileManager.default.removeItem(at: url)
    movie.startRecording(to: url, recordingDelegate: self)
    check(waitFor(1.5) { evCount("movieStarted") > 0 }, "didStartRecording")
    usleep(1_500_000)
    let recDur = movie.recordedDuration.seconds
    movie.stopRecording()
    let finishS = 4.0, lateDrainS = 8.0   // the recording finishes within finishS; never, if not after lateDrainS more
    let fin = waitFor(finishS) { lock.withLock { movieDone != nil } }
    check(fin, timing: true, "didFinishRecording within \(secondsText(finishS)) s")
    if !fin, !waitFor(lateDrainS, { lock.withLock { movieDone != nil } }) {
      check(false, "recording never finished (\(secondsText(finishS + lateDrainS)) s)")
    }
    if let (u, e) = movieDone {
      check(e == nil, "recording error \(String(describing: e))")
      let a = AVURLAsset(url: u)
      let sem = DispatchSemaphore(value: 0)
      var dur = 0.0
      var vt: [String] = []
      Task {
        do {
          dur = try await a.load(.duration).seconds
          for t in try await a.loadTracks(withMediaType: .video) {
            let s = try await t.load(.naturalSize)
            vt.append("\(Int(s.width))x\(Int(s.height))")
          }
        } catch { vt.append("err \(error)") }
        sem.signal()
      }
      _ = sem.wait(timeout: .now() + 3)
      check(vt.contains("\(dm.width)x\(dm.height)") || vt.contains("\(dm.height)x\(dm.width)"),
            "movie track \(vt) matches the active format \(dm.width)x\(dm.height)")
      check(dur > 1.0 && dur < 3.0 && !vt.isEmpty,
            "movie duration \(String(format: "%.2f", dur)) s video tracks \(vt) recordedDuration \(String(format: "%.2f", recDur))")
    }
  }
  // 7b. a capture followed at once by stopRunning (shutter, then leave the screen): the photo and both completions must
  // arrive, and didFinishCaptureFor must come before the session's didStopRunning notification (stopRunning waits
  // for in-flight photos; without that wait AVFoundation still delivers the photo, but after the stop).
  func photoThenStop(_ a: MainSession) {
    let session = a.session, photo = a.photo
    stage = "photo-then-stop"
    let stopsBefore = evCount("didStop")
    lock.withLock {
      photoEvents = []
      photos = []
      photoDone = false
    }
    photo.capturePhoto(with: AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg]), delegate: self)
    let tStop = Date()
    session.stopRunning()   // at once, so the encode is still in flight
    let tRet = Date()
    let stopDone = waitFor(15) { lock.withLock { photoDone } }
    let (stopEvents, stopPhotos) = lock.withLock { (photoEvents, photos) }
    check(stopDone, "didFinishCaptureFor after capture + stopRunning (events \(stopEvents))")
    check(stopPhotos.count == 1 && stopPhotos[0].err == nil && stopPhotos[0].data != nil,
          "photo delivered without error after stopRunning (n=\(stopPhotos.count) err=\(String(describing: stopPhotos.first?.err)))")
    check(waitFor(2) { evCount("didStop") > stopsBefore }, "didStopRunning notification after capture + stopRunning")
    let stopsAtCompletion = lock.withLock { stopsAtFinish }
    check(stopDone && stopsAtCompletion == stopsBefore,
          "didFinishCaptureFor before didStopRunning (stop notifications at completion: \(stopsAtCompletion - stopsBefore))")
    // The engine reports the stop once the photo's didFinishCaptureFor has run, bounded at 500 ms. A stop that returns
    // ~500 ms after that callback means the wait ran to its bound (the done-mark never came) and the order check above
    // passed by the bound alone, so the threshold is half the bound. Timed from the callback, not from the call: the
    // call also includes the encode (100-350 ms) and any late callback (the 300 ms positive control).
    let finAt = lock.withLock { finishAt }
    if stopDone && stopsAtCompletion == stopsBefore {
      let gap = tRet.timeIntervalSince(finAt)
      check(gap < 0.25, timing: true,
            "stopRunning returned \(Int(gap * 1000)) ms after didFinishCaptureFor"
              + " (< 250 ms; the call took \(Int(tRet.timeIntervalSince(tStop) * 1000)) ms)")
    }
    let st0 = evCount("didStart")
    session.startRunning()   // no frame check here: no VideoDataOutput is attached at this point (the bgfg stage adds one)
    check(waitFor(2) { evCount("didStart") > st0 }, "didStartRunning notification after the restart")
  }
  // 8. background / foreground (runner backgrounds the app on BG-REQUEST; else simulate with stop/start)
  func backgroundForeground(_ a: MainSession, _ movie: AVCaptureMovieFileOutput) {
    let session = a.session
    stage = "bgfg"
    let stops0 = evCount("didStop")
    session.beginConfiguration()
    session.removeOutput(movie)
    let vdo2 = AVCaptureVideoDataOutput()
    keptUntilExit.append(vdo2)
    vdo2.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.vdo2"))
    session.addOutput(vdo2)
    session.commitConfiguration()
    log("BG-REQUEST")
    if waitFor(5, { bgSeen }) {
      session.stopRunning()   // what apps do in sceneDidEnterBackground
      _ = waitFor(5) { fgSeen }
      check(fgSeen, "foreground notification after background")
      usleep(300_000)
    } else {
      log("no real background within 5 s -> simulated (stop/start)")
      session.stopRunning()
    }
    check(waitFor(1) { evCount("didStop") > stops0 }, "didStopRunning notification")
    session.startRunning()
    firstFrame("after-fg")
    resetFrames()
    usleep(800_000)
    checkFrames("after-fg", minCount: 10)
    check(waitFor(1) { previewHasContents() }, "preview has contents after foreground")
  }
  // 9. teardown + a second session (lifetime / dangling-pointer bugs surface here)
  func teardown(_ a: MainSession) {
    let session = a.session
    stage = "teardown"
    session.stopRunning()
    for o in session.outputs { session.removeOutput(o) }
    for i in session.inputs { session.removeInput(i) }
    onMain {
      preview?.session = nil
      preview?.removeFromSuperlayer()
      preview = nil
    }
    obs = []
    usleep(300_000)
  }
  func secondSessions(_ dev: AVCaptureDevice) {
    stage = "second-session"
    // ledger L37: three sessions in a row, each released before the next, so one can be allocated at a freed one's
    // address (an earlier bug: it inherited the dead session's sinks). The first session above is still alive, so S2
    // alone never did.
    for n in 2...4 {
      autoreleasepool {
        let s2 = AVCaptureSession()
        s2.beginConfiguration()
        if let i2 = try? AVCaptureDeviceInput(device: dev), s2.canAddInput(i2) {
          s2.addInput(i2)
        } else {
          check(false, "session \(n) input")
        }
        let v3 = AVCaptureVideoDataOutput()
        v3.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.vdo\(n)"))
        s2.addOutput(v3)
        s2.commitConfiguration()
        s2.startRunning()
        firstFrame("S\(n)")
        resetFrames()
        usleep(n == 2 ? 700_000 : 400_000)
        checkFrames("S\(n)", minCount: n == 2 ? 8 : 4)
        s2.stopRunning()
        for o in s2.outputs { s2.removeOutput(o) }
        for i in s2.inputs { s2.removeInput(i) }
      }
      usleep(150_000)
    }
  }
}

let canary = Canary()
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
    guard let ws = scene as? UIWindowScene else { return }
    let w = UIWindow(windowScene: ws)
    w.rootViewController = UIViewController()
    w.rootViewController!.view.backgroundColor = .black
    w.makeKeyAndVisible()
    window = w
    if canaryStarted { return }
    canaryStarted = true
    canary.window = w
    DispatchQueue.global().asyncAfter(deadline: .now() + budget) {
      // a content FAIL recorded before the budget ran out stays a FAIL; only a run with none is INCONCLUSIVE
      let (cf, ef) = logLock.withLock { (coreFail, extFail) }
      log("RESULT core=\(cf.isEmpty ? "INCONCLUSIVE" : "FAIL") ext=\(ef.isEmpty ? "INCONCLUSIVE" : "FAIL") watchdog stage=\(stage) after \(budget)s"
          + retriedField()
          + (cf.isEmpty ? "" : " core:[\(cf.joined(separator: "; "))]")
          + (ef.isEmpty ? "" : " ext:[\(ef.joined(separator: "; "))]"))
      exit(cf.isEmpty ? 3 : 1)
    }
    log("start budget=\(budget)s")
    Thread { canary.run() }.start()
  }
}
final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
    true
  }
  func application(_ a: UIApplication, configurationForConnecting s: UISceneSession,
                   options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: "Default", sessionRole: s.role)
    c.delegateClass = SceneDelegate.self
    return c
  }
}
var canaryStarted = false
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
