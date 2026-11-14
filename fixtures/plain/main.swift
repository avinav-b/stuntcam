// SPDX-License-Identifier: Apache-2.0
// m5 Q4: plain AVFoundation checks on genuine devices. Mode via env M5APP_MODE = scan | q4
import UIKit
import AVFoundation
import CoreMotion
import os

func P(_ s: String) { print("M5APP| " + s); fflush(stdout) }
let allTypes: [AVCaptureDevice.DeviceType] = [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera, .builtInDualCamera,
  .builtInDualWideCamera, .builtInTripleCamera, .builtInTrueDepthCamera, .builtInLiDARDepthCamera, .continuityCamera, .external]
func pos(_ p: AVCaptureDevice.Position) -> String { ["unspec", "back", "front"][p.rawValue] }
func short(_ t: AVCaptureDevice.DeviceType) -> String { t.rawValue.replacingOccurrences(of: "AVCaptureDeviceType", with: "") }
func desc(_ d: AVCaptureDevice) -> String {
  "\(d.uniqueID) '\(d.localizedName)' type=\(short(d.deviceType)) pos=\(pos(d.position)) virtual=\(d.isVirtualDevice) constituents=[\(d.constituentDevices.map { short($0.deviceType) }.joined(separator: ","))] formats=\(d.formats.count) switchOver=\(d.virtualDeviceSwitchOverVideoZoomFactors)"
}

final class VC: UIViewController {
  var obs: [NSKeyValueObservation] = []
  var coord: AVCaptureDevice.RotationCoordinator?
  lazy var session = AVCaptureSession()   // lazy: the default run does discovery before any session exists (M5APP_ORDER=session flips it)
  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    let mode = ProcessInfo.processInfo.environment["M5APP_MODE"] ?? "m9"
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
      let run = { mode == "scan" ? self.scan() : mode == "m9" ? self.m9() : mode == "cycle" ? self.cycle() : self.q4() }
      if let order = ProcessInfo.processInfo.environment["M5APP_ORDER"] { self.ordering(order, then: run) } else { run() }
    }
  }
  var motion: [CMMotionManager] = []; var motionCounts: [Int] = []
  func done() { DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
    if !self.motion.isEmpty { P("INFO motion.end counts=\(self.motionCounts) pull=\(self.motion.first?.accelerometerData != nil)") }
    P("DONE"); exit(0) } }

  func scan() {
    let ds = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified)
    P("discovery all types: \(ds.devices.count)")
    for d in ds.devices { P("  " + desc(d)) }
    for t in allTypes {
      let n = AVCaptureDevice.DiscoverySession(deviceTypes: [t], mediaType: .video, position: .unspecified).devices.map { $0.uniqueID }
      P("  only[\(short(t))] -> \(n)")
    }
    done()
  }

  func q4() {
    let ds = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified)
    P("devices \(ds.devices.count)"); for d in ds.devices { P("  " + desc(d)) }
    P("supportedMultiCamDeviceSets \(ds.supportedMultiCamDeviceSets.map { $0.map { short($0.deviceType) } })  isMultiCamSupported=\(AVCaptureMultiCamSession.isMultiCamSupported)")
    for t in [AVCaptureDevice.DeviceType.builtInTripleCamera, .builtInDualWideCamera, .builtInDualCamera, .builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera, .builtInLiDARDepthCamera, .builtInTrueDepthCamera] {
      P("default(\(short(t)), back) = \(AVCaptureDevice.default(t, for: .video, position: .back)?.uniqueID ?? "nil")  front = \(AVCaptureDevice.default(t, for: .video, position: .front)?.uniqueID ?? "nil")")
    }
    P("default(for: .video) = \(AVCaptureDevice.default(for: .video).map(desc) ?? "nil")")
    P("systemPreferredCamera = \(AVCaptureDevice.systemPreferredCamera.map(desc) ?? "nil")")
    P("userPreferredCamera (before) = \(AVCaptureDevice.userPreferredCamera.map(desc) ?? "nil")")
    if let front = ds.devices.first(where: { $0.position == (ProcessInfo.processInfo.environment["M5APP_PREF"] == "back" ? .back : .front) }) {
      AVCaptureDevice.userPreferredCamera = front
      P("userPreferredCamera := front -> user=\(AVCaptureDevice.userPreferredCamera?.uniqueID ?? "nil") system=\(AVCaptureDevice.systemPreferredCamera?.uniqueID ?? "nil")")
    }
    for d in ds.devices.prefix(3) { P("AVCaptureDevice(uniqueID: \(d.uniqueID)) -> \(AVCaptureDevice(uniqueID: d.uniqueID).map { $0 === d ? "same object" : "other object \($0)" } ?? "nil")") }
    P("AVCaptureDevice(uniqueID: bogus) -> \(String(describing: AVCaptureDevice(uniqueID: "bogus")))")

    // formats as VC sees them
    if let back = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) {
      for f in back.formats {
        let dims = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
        let sub = CMFormatDescriptionGetMediaSubType(f.formatDescription)
        let fcc = String(bytes: [UInt8(sub >> 24 & 255), UInt8(sub >> 16 & 255), UInt8(sub >> 8 & 255), UInt8(sub & 255)], encoding: .ascii)!
        P("  fmt \(dims.width)x\(dims.height) \(fcc) fps=\(f.videoSupportedFrameRateRanges.map { "\($0.minFrameRate)-\($0.maxFrameRate)" }) photoDims=\(f.supportedMaxPhotoDimensions.map { "\($0.width)x\($0.height)" }) stab=\([AVCaptureVideoStabilizationMode.standard, .cinematic, .cinematicExtended, .auto].filter { f.isVideoStabilizationModeSupported($0) }.map { $0.rawValue }) hdr=\(f.isVideoHDRSupported) colorSpaces=\(f.supportedColorSpaces.map { $0.rawValue }) depth=\(f.supportedDepthDataFormats.count) binned=\(f.isVideoBinned) fov=\(f.videoFieldOfView) maxZoom=\(f.videoMaxZoomFactor) multiCam=\(f.isMultiCamSupported) highPhotoQ=\(f.isHighPhotoQualitySupported)")
      }
      // Rotation coordinator with a preview layer attached to a running session
      do {
        let input = try AVCaptureDeviceInput(device: back)
        session.beginConfiguration(); if session.canAddInput(input) { session.addInput(input) }
        let out = AVCapturePhotoOutput(); if session.canAddOutput(out) { session.addOutput(out) }
        session.commitConfiguration()
        let layer = AVCaptureVideoPreviewLayer(session: session); layer.frame = view.bounds; layer.videoGravity = .resizeAspectFill
        view.layer.addSublayer(layer)
        DispatchQueue.global().async { self.session.startRunning(); P("session running=\(self.session.isRunning)") }
        let c = AVCaptureDevice.RotationCoordinator(device: back, previewLayer: layer)
        coord = c
        P("RotationCoordinator preview=\(c.videoRotationAngleForHorizonLevelPreview) capture=\(c.videoRotationAngleForHorizonLevelCapture) previewLayerConn=\(String(describing: layer.connection?.videoRotationAngle))")
        obs.append(c.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.new]) { _, ch in P("KVO preview angle -> \(ch.newValue ?? -1)") })
        obs.append(c.observe(\.videoRotationAngleForHorizonLevelCapture, options: [.new]) { _, ch in P("KVO capture angle -> \(ch.newValue ?? -1)") })
        // rotate UI to landscape to see whether the coordinator follows (it reads device orientation)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
          if let ws = self.view.window?.windowScene {
            ws.requestGeometryUpdate(.iOS(interfaceOrientations: .landscapeRight)) { e in P("geometry update error \(e)") }
            P("requested landscapeRight")
          }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) {
          P("after rotate: UIDevice.orientation=\(UIDevice.current.orientation.rawValue) iface=\(self.view.window?.windowScene?.effectiveGeometry.interfaceOrientation.rawValue ?? -1) preview=\(c.videoRotationAngleForHorizonLevelPreview) capture=\(c.videoRotationAngleForHorizonLevelCapture)")
          self.done()
        }
      } catch { P("input error \(error)"); done() }
    } else { done() }
  }
}
// ---- m9: plain-AVF step matrix (one line per step: PASS/FAIL name detail) ----
final class M9: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate {
  var frames = 0; var firstFrame: String?; var photoCB: ((AVCapturePhoto?, Error?) -> Void)?; var movieCB: ((URL, Error?) -> Void)?
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    frames += 1
    if firstFrame == nil, let pb = CMSampleBufferGetImageBuffer(sb) { firstFrame = "\(CVPixelBufferGetWidth(pb))x\(CVPixelBufferGetHeight(pb))" }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto p: AVCapturePhoto, error: Error?) { E("photo.processing", error); photoCB?(p, error) }
  let finished = DispatchSemaphore(value: 0)   // m10: a plain app that waits for didFinishCaptureFor (hung forever before m10)
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishCaptureFor r: AVCaptureResolvedPhotoSettings, error: Error?) { E("photo.didFinishCapture", error); finished.signal() }
  func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo u: URL, from c: [AVCaptureConnection], error: Error?) { E("movie.didFinishRecording", error); movieCB?(u, error) }
}
// w2: the NSError a callback got, field by field (maps engine statuses to app-visible errors, design §5.4); nothing when nil
func E(_ cb: String, _ e: Error?) {
  guard let e else { return }; let n = e as NSError
  let ui = n.userInfo.keys.sorted().map { k in "\(k)=\(n.userInfo[k].map { "\($0)" } ?? "nil")".replacingOccurrences(of: "\n", with: " ") }.joined(separator: "; ")
  let u = n.userInfo[NSUnderlyingErrorKey] as? NSError
  P("NSERROR \(cb) domain=\(n.domain) code=\(n.code) desc='\(n.localizedDescription)' underlying=\(u.map { "\($0.domain)/\($0.code)" } ?? "-") userInfo={\(ui)}")
}
func R(_ ok: Bool, _ name: String, _ d: String = "") { P("\(ok ? "PASS" : "FAIL") \(name) \(d)") }
extension VC {
  func m9() {
    let m = M9(); objc_setAssociatedObject(self, "m9", m, .OBJC_ASSOCIATION_RETAIN)
    P("os=\(UIDevice.current.systemVersion)")
    let t0 = Date(); armW4Timers()
    let ds = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified)
    firstCallDone(t0)
    if expectStock {   // W4: activation is expected to end stock (no camera, no crash, no hang)
      R(ds.devices.isEmpty, "stock.discovery", "count=\(ds.devices.count)")
      let tl = Date(); let dv = AVCaptureDevice.default(for: .video); P("INFO latercall.ms=\(String(format: "%.2f", Date().timeIntervalSince(tl) * 1000))")
      R(dv == nil, "stock.default", "default(for: .video) is nil")
      done(); return
    }
    R(ds.devices.count > 0, "discovery", "count=\(ds.devices.count) \(ds.devices.map { "\($0.localizedName)(\(type(of: $0)))" })")
    guard let back = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else { R(false, "default.back"); done(); return }
    R(true, "default.back", back.uniqueID)
    R(back.formats.count > 0, "formats", "count=\(back.formats.count) photoMax=\(back.formats.flatMap { $0.supportedMaxPhotoDimensions }.map { "\($0.width)x\($0.height)" }.max() ?? "-")")
    do { try back.lockForConfiguration(); back.videoZoomFactor = 2; R(back.videoZoomFactor == 2, "lock+zoom", "zoom=\(back.videoZoomFactor)")
      if back.hasTorch { back.torchMode = .on }; R(back.hasTorch, "torch", "hasTorch=\(back.hasTorch) mode=\(back.torchMode.rawValue)")
      back.videoZoomFactor = 1; back.torchMode = .off; back.unlockForConfiguration() } catch { R(false, "lock", "\(error)") }
    var kvo = 0; obs.append(back.observe(\.isAdjustingFocus, options: [.new]) { _, _ in kvo += 1 })
    do {
      let input = try AVCaptureDeviceInput(device: back)
      session.beginConfiguration(); session.sessionPreset = .photo
      if session.canAddInput(input) { session.addInput(input) }
      let vdo = AVCaptureVideoDataOutput(); vdo.setSampleBufferDelegate(m, queue: DispatchQueue(label: "vdo")); if session.canAddOutput(vdo) { session.addOutput(vdo) }
      let po = AVCapturePhotoOutput(); if session.canAddOutput(po) { session.addOutput(po) }
      let mo = AVCaptureMovieFileOutput(); if session.canAddOutput(mo) { session.addOutput(mo) }
      session.commitConfiguration()
      R(session.outputs.count == 3, "session.config", "inputs=\(session.inputs.count) outputs=\(session.outputs.count)")
      let layer = AVCaptureVideoPreviewLayer(session: session); layer.frame = view.bounds; layer.videoGravity = .resizeAspectFill; view.layer.addSublayer(layer)
      DispatchQueue.global().async {
        let t0 = Date(); self.session.startRunning(); let ms = Int(Date().timeIntervalSince(t0) * 1000)
        R(self.session.isRunning, "session.start", "ms=\(ms)")
        // w1: count the 2 s window from the first frame, not from startRunning (a cold start on a hosted runner gave 10 frames)
        let tf = Date(); while m.frames == 0 && Date().timeIntervalSince(tf) < 5 { Thread.sleep(forTimeInterval: 0.02) }
        let ffms = Int(Date().timeIntervalSince(tf) * 1000); let f0 = m.frames
        Thread.sleep(forTimeInterval: 2.0)
        R(m.frames - f0 > 20, "frames", "n=\(m.frames - f0) first=\(m.firstFrame ?? "-") firstFrameAfterStart=\(ffms)ms")
        DispatchQueue.main.sync { R(layer.contents != nil || layer.sublayers?.isEmpty == false, "preview.contents", "contents=\(layer.contents.map { "\(type(of: $0))" } ?? "nil")") }
        try? back.lockForConfiguration(); if back.isFocusPointOfInterestSupported { back.focusPointOfInterest = CGPoint(x: 0.5, y: 0.5); back.focusMode = .autoFocus }; back.unlockForConfiguration()
        Thread.sleep(forTimeInterval: 1.0); R(kvo >= 2, "focus.kvo", "changes=\(kvo)")
        let sem = DispatchSemaphore(value: 0)
        // w4 e40: M5APP_PHOTOSTOP=bg|main|incallback: a photo, then stopRunning while it is in flight (bg: on this thread;
        // main: on the main thread, the photo delegate queue; incallback: inside didFinishProcessingPhoto). The photo and its
        // didFinishCapture must arrive, the stop must neither hang nor wait the engine's 1 s bound (a stop on the delegate
        // queue can't wait for callbacks queued behind it), and from bg didFinishCapture comes before stopRunning returns.
        // w4 e44 (review): mainsync: main does `sessionQueue.sync { stopRunning() }` (the main thread under another
        // queue; M1); restart: stop on this thread and, 30 ms later, startRunning from another thread while the stop still
        // waits for the photo's callbacks (L4). AVF drops that overlapping start on 17.5/26.3/27.0 (e44), so the case checks
        // the session stays consistent and that a later start brings frames back. isRunning is false when stopRunning returns
        // (not checked for restart, where the start may already have run).
        // w4 e48: M5APP_PHOTOS=<n>: n JPEG photos one after another (each waits for didFinishCapture, then 300 ms), one INFO
        // line each with its settings uniqueID, AVCapturePhoto.timestamp, the host clock at the shutter and the latency
        // (capturePhoto -> didFinishProcessingPhoto); then, with M5APP_HOLD=<s>, "HOLD" and s seconds with the session running
        // (so `stuntcam trace` can read a running app), then stop and DONE
        if let np = Int(ProcessInfo.processInfo.environment["M5APP_PHOTOS"] ?? ""), np > 0 {
          var lats: [Double] = []
          for i in 1...np {
            let st = AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg])
            let tl = NSLock(); var ts = -1.0, exif = "-"
            m.photoCB = { p, e in
              // w4 e54: the photo's EXIF capture time (DateTimeOriginal, SubsecTimeOriginal, OffsetTimeOriginal) as written
              var x = "-"
              if let d = p?.fileDataRepresentation(), let src = CGImageSourceCreateWithData(d as CFData, nil),
                 let pr = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [String: Any], let ex = pr[kCGImagePropertyExifDictionary as String] as? [String: Any] {
                x = "\(ex[kCGImagePropertyExifDateTimeOriginal as String] ?? "-")|\(ex[kCGImagePropertyExifSubsecTimeOriginal as String] ?? "-")|\(ex[kCGImagePropertyExifOffsetTimeOriginal as String] ?? "-")"
              }
              tl.lock(); ts = p?.timestamp.seconds ?? -1; exif = x.replacingOccurrences(of: " ", with: "_"); tl.unlock()
              R(e == nil && (p?.fileDataRepresentation()?.count ?? 0) > 0, "photos.\(i)", "err=\(String(describing: e))"); sem.signal() }
            let h0 = CMClockGetTime(CMClockGetHostTimeClock()).seconds, t = Date()
            po.capturePhoto(with: st, delegate: m)
            if sem.wait(timeout: .now() + 8) == .timedOut { R(false, "photos.\(i)", "timeout") }
            let ms = Date().timeIntervalSince(t) * 1000; lats.append(ms)
            R(m.finished.wait(timeout: .now() + 3) == .success, "photos.\(i).didFinishCapture", "sid=\(st.uniqueID)")
            tl.lock(); let t1 = ts, x1 = exif; tl.unlock()
            P("INFO photo \(i) sid=\(st.uniqueID) ts=\(String(format: "%.6f", t1)) shutterHost=\(String(format: "%.6f", h0)) latencyMs=\(String(format: "%.1f", ms)) exif=\(x1)")
            Thread.sleep(forTimeInterval: 0.3)
          }
          let sl = lats.sorted()
          P("INFO photos n=\(np) latencyMs median=\(String(format: "%.1f", sl[sl.count / 2])) min=\(String(format: "%.1f", sl.first ?? 0)) max=\(String(format: "%.1f", sl.last ?? 0))")
          if let hold = Double(ProcessInfo.processInfo.environment["M5APP_HOLD"] ?? ""), hold > 0 { P("HOLD \(hold) s"); Thread.sleep(forTimeInterval: hold) }
          self.session.stopRunning(); R(!self.session.isRunning, "session.stop")
          DispatchQueue.main.async { self.done() }; return
        }
        if let ps = ProcessInfo.processInfo.environment["M5APP_PHOTOSTOP"] {
          var stopMs = -1, finishedFirst = false, stoppedAtReturn = false
          let stopNow = { let t = Date(); self.session.stopRunning(); stopMs = Int(Date().timeIntervalSince(t) * 1000); stoppedAtReturn = !self.session.isRunning }
          let nl = NSLock(); var notes: [String] = []; var nobs: [NSObjectProtocol] = []
          for (n, k) in [(AVCaptureSession.didStartRunningNotification, "start"), (AVCaptureSession.didStopRunningNotification, "stop")] {
            nobs.append(NotificationCenter.default.addObserver(forName: n, object: self.session, queue: nil) { _ in nl.lock(); notes.append(k); nl.unlock() })
          }
          m.photoCB = { p, e in
            if ps == "incallback" { stopNow() }
            R(e == nil && (p?.fileDataRepresentation()?.count ?? 0) > 0, "photostop.photo", "\(ps) err=\(String(describing: e))"); sem.signal() }
          po.capturePhoto(with: AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg]), delegate: m)
          let startDone = DispatchSemaphore(value: 0); var startMs = -1
          switch ps {
          case "main": DispatchQueue.main.sync { stopNow() }
          case "mainsync": let sq = DispatchQueue(label: "m5app.session"); DispatchQueue.main.sync { sq.sync { stopNow() } }
          case "bg": stopNow(); finishedFirst = m.finished.wait(timeout: .now()) == .success; if finishedFirst { m.finished.signal() }
          case "restart":
            DispatchQueue.global().asyncAfter(deadline: .now() + 0.03) { let t = Date(); self.session.startRunning(); startMs = Int(Date().timeIntervalSince(t) * 1000); startDone.signal() }
            stopNow()
          default: break
          }
          if sem.wait(timeout: .now() + 8) == .timedOut { R(false, "photostop.photo", "\(ps) timeout") }
          R(m.finished.wait(timeout: .now() + 3) == .success, "photostop.didFinishCapture", ps)
          R(stopMs >= 0 && stopMs < 900, "photostop.stop", "\(ps) stopRunning took \(stopMs) ms (< 900)")
          if ps == "restart" {
            // AVF (17.5, 26.3, 27.0; e44) doesn't forward a startRunning that overlaps a stopRunning still waiting for its
            // DidStopRunning: isRunning is still true then, and the start returns without starting. So the overlapping start
            // must leave a coherent session (isRunning agrees with frames; no DidStart then DidStop), and a start after the
            // stop must resume frames.
            let started = startDone.wait(timeout: .now() + 5) == .success
            let f0 = m.frames; Thread.sleep(forTimeInterval: 1.0); let df0 = m.frames - f0; let run0 = self.session.isRunning
            nl.lock(); let ns0 = notes; nl.unlock()
            P("INFO photostop.restart overlapping startRunning took \(startMs) ms; then isRunning \(run0), \(df0) frames/s, notifications \(ns0)")
            R(started && (run0 ? df0 > 10 : df0 == 0), "photostop.restart.coherent", "isRunning \(run0) with \(df0) frames in 1 s")
            R(!(ns0.firstIndex(of: "start").map { i in ns0[i...].contains("stop") } ?? false), "photostop.restart.order", "session notifications \(ns0) (no DidStart then DidStop)")
            if !run0 { self.session.startRunning() }
            let f1 = m.frames; Thread.sleep(forTimeInterval: 1.0)
            nl.lock(); let ns = notes; nl.unlock()
            R(self.session.isRunning && m.frames - f1 > 10, "photostop.restart.frames", "\(m.frames - f1) frames in the 1 s after the restart, isRunning \(self.session.isRunning)")
            R(ns.last == "start", "photostop.restart.last", "session notifications \(ns)")
            self.session.stopRunning()
          } else {
            R(stoppedAtReturn, "photostop.stoppedAtReturn", "\(ps) isRunning false when stopRunning returned")
            R(!self.session.isRunning, "photostop.stopped", ps)
          }
          if ps == "bg" { R(finishedFirst, "photostop.order", "didFinishCapture before stopRunning returned") }
          nobs.forEach { NotificationCenter.default.removeObserver($0) }
          DispatchQueue.main.async { self.done() }; return
        }
        m.photoCB = { p, e in
          let d = p?.fileDataRepresentation(); var dims = "-"
          if let d, let src = CGImageSourceCreateWithData(d as CFData, nil), let pr = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [String: Any] { dims = "\(pr["PixelWidth"] ?? "?")x\(pr["PixelHeight"] ?? "?") orient=\(pr["Orientation"] ?? "?")" }
          R(e == nil && (d?.count ?? 0) > 0, "photo", "err=\(String(describing: e)) bytes=\(d?.count ?? 0) dims=\(dims)"); sem.signal() }
        po.capturePhoto(with: AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg]), delegate: m)
        if sem.wait(timeout: .now() + 8) == .timedOut { R(false, "photo", "timeout") }
        R(m.finished.wait(timeout: .now() + 3) == .success, "photo.didFinishCapture", "waited on captureOutput:didFinishCaptureForResolvedSettings:")
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("m9.mov"); try? FileManager.default.removeItem(at: url)
        m.movieCB = { u, e in let a = AVURLAsset(url: u); let dur = CMTimeGetSeconds(a.duration)
          R(dur > 1.0, "movie", "err=\(String(describing: e)) dur=\(String(format: "%.2f", dur)) tracks=\(a.tracks.count)"); sem.signal() }
        mo.startRecording(to: url, recordingDelegate: m); Thread.sleep(forTimeInterval: 2.0); mo.stopRecording()
        if sem.wait(timeout: .now() + 8) == .timedOut { R(false, "movie", "timeout") }
        self.session.stopRunning(); R(!self.session.isRunning, "session.stop")
        DispatchQueue.main.async { self.done() }
      }
    } catch { R(false, "input", "\(error)"); done() }
  }
}
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo s: UISceneSession, options o: UIScene.ConnectionOptions) {
    guard let ws = scene as? UIWindowScene else { return }
    window = UIWindow(windowScene: ws); window!.rootViewController = VC(); window!.makeKeyAndVisible()
  }
}
final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool {
    UIDevice.current.beginGeneratingDeviceOrientationNotifications(); return true
  }
  func application(_ a: UIApplication, configurationForConnecting s: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: nil, sessionRole: s.role); c.delegateClass = SceneDelegate.self; return c
  }
}
// ---- w1 ordering fixtures (design §10.3, U2): what the app touches before its first camera call.
//   M5APP_ORDER=motion  : CoreMotion first (one manager, push updates at 100 Hz), then the camera steps
//   M5APP_ORDER=motion2 : a two-manager motion recorder: a pull manager plus a push manager on an OperationQueue, both kept running
//                         through the camera steps; gyro/magnetometer/deviceMotion availability reported (INFO, not gated yet)
//   M5APP_ORDER=audio   : AVCaptureDevice.default(for: .audio) first
//   M5APP_ORDER=session : an AVCaptureSession exists before discovery
//   M5APP_ORDER=threads : three threads make their first capture call at once (U13), then the camera steps
//   M5APP_ORDER=motion-main-camera-bg : (e29) the first motion call on main; a background thread makes the first camera call
//                         0.3 s later (M16_ORIENT_FAULT=start|runmain wait in that main-thread call until it arms)
//   M5APP_ORDER=motion-subclass-camera : (e31) a CMMotionManager subclass whose accelerometerUpdateInterval getter makes the
//                         app's first camera call (on main): the stand-in reads that getter inside its motion scope, the
//                         dynamic send review round 2 low 3 is about (with M16_TRAP_FAULT the trap faults there)
final class CamIntervalManager: CMMotionManager {
  var camCalled = false
  override var accelerometerUpdateInterval: TimeInterval {
    get {
      if !camCalled {
        camCalled = true
        let n = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).devices.count
        P("INFO motion-subclass-camera discovery=\(n) (from the interval getter)")
      }
      return super.accelerometerUpdateInterval
    }
    set { super.accelerometerUpdateInterval = newValue }
  }
}
extension VC {
  func ordering(_ order: String, then run: @escaping () -> Void) {
    P("order=\(order)")
    switch order {
    case "motion", "motion2":
      let hz = 100.0
      let push = CMMotionManager(); push.accelerometerUpdateInterval = 1 / hz
      R(push.isAccelerometerAvailable, "order.motion.available", "accel=\(push.isAccelerometerAvailable)")
      P("INFO order.motion.others gyro=\(push.isGyroAvailable) mag=\(push.isMagnetometerAvailable) dm=\(push.isDeviceMotionAvailable)")
      motion = [push]; motionCounts = [0]
      let q = OperationQueue(); q.maxConcurrentOperationCount = 1
      var badTs = 0
      push.startAccelerometerUpdates(to: q) { d, _ in
        guard let d else { return }
        let up = ProcessInfo.processInfo.systemUptime
        if abs(up - d.timestamp) > 1.0 { badTs += 1 }
        DispatchQueue.main.async { self.motionCounts[0] += 1 }
      }
      if order == "motion2" {
        let pull = CMMotionManager(); pull.accelerometerUpdateInterval = 1 / hz; pull.startAccelerometerUpdates()
        motion.insert(pull, at: 0)
      }
      DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
        let n = self.motionCounts[0]; let want = Int(hz * 1.5)
        R(Double(n) >= 0.9 * Double(want), "order.motion.rate", "updates=\(n) want>=\(Int(0.9 * Double(want))) badTimestamps=\(badTs)")
        R(badTs == 0, "order.motion.timestamps", "outside systemUptime±1s: \(badTs)")
        if order == "motion2" {
          let a = self.motion[0].accelerometerData
          R(a != nil, "order.motion2.pull", "accel=\(a.map { "\($0.acceleration.x),\($0.acceleration.y),\($0.acceleration.z)" } ?? "nil")")
        }
        run()
      }
    case "motion-only":   // W4: CoreMotion only, never the camera
      motionChecks(order) { self.done() }
    case "camera-then-motion":   // W4: the first camera call first (with SIMCAM_PF_BREAK: stock), then CoreMotion in the same launch
      let t0 = Date(); armW4Timers()
      let n = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).devices.count
      firstCallDone(t0)
      P("INFO camera-then-motion discovery=\(n)")
      motionChecks(order) { self.done() }
    case "motion-threads":   // W4: three threads make their first motion query at once
      let g = DispatchGroup(); let start = DispatchSemaphore(value: 0); let lock = NSLock()
      var qms = [Double](repeating: -1, count: 3); var counts = [0, 0, 0]
      let mgrs = (0..<3).map { _ in CMMotionManager() }; let q = OperationQueue()
      for i in 0..<3 {
        DispatchQueue.global().async(group: g) {
          start.wait(); let t = Date()
          let m = mgrs[i]; m.accelerometerUpdateInterval = 0.01
          _ = m.isAccelerometerAvailable
          m.startAccelerometerUpdates(to: q) { d, _ in if d != nil { lock.lock(); counts[i] += 1; lock.unlock() } }
          let d = Date().timeIntervalSince(t) * 1000; lock.lock(); qms[i] = d; lock.unlock()
        }
      }
      for _ in 0..<3 { start.signal() }
      _ = g.wait(timeout: .now() + 5)
      DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
        lock.lock(); let c = counts; let m = qms; lock.unlock()
        // M5APP_EXPECT_MOTION=stock (review): three threads hit the forwarding traps at once; stock delivers nothing
        if expectMotionStock { R(c.allSatisfy { $0 == 0 }, "order.motion-threads.stock.updates", "counts=\(c)") }
        else { R(c.allSatisfy { $0 >= 90 }, "order.motion-threads.updates", "counts=\(c)") }
        R(m.allSatisfy { $0 >= 0 && $0 <= 5 }, "order.motion-threads.firstquery", "ms=\(m.map { String(format: "%.2f", $0) })")
        for mg in mgrs { mg.stopAccelerometerUpdates() }
        self.done()
      }
    case "motion-main-camera-bg":   // e29: the first motion call on main while another thread arms the camera 0.3 s later
      Thread.detachNewThread {
        Thread.sleep(forTimeInterval: 0.3)
        let n = AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).devices.count
        P("INFO motion-main-camera-bg discovery=\(n)")
      }
      motionChecks(order) { self.done() }
    case "motion-subclass-camera":   // e31: the first camera call from an app getter the stand-in reads (see above)
      let m = CamIntervalManager(); m.accelerometerUpdateInterval = 0.01; motion = [m]
      var n = 0
      m.startAccelerometerUpdates(to: .main) { d, _ in if d != nil { n += 1 } }
      R(m.camCalled, "order.motion-subclass-camera.getter", "the stand-in read the subclass's interval getter: \(m.camCalled)")
      DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
        R(n >= 135, "order.motion-subclass-camera.updates", "updates=\(n) in 1.5 s at 100 Hz, want>=135")
        m.stopAccelerometerUpdates(); self.done()
      }
    case "none":   // W4: neither camera nor motion
      P("INFO none: idle 2 s"); DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self.done() }
    case "audio":
      let mic = AVCaptureDevice.default(for: .audio)
      R(mic != nil, "order.audio.first", "mic=\(mic.map { "\($0.uniqueID) '\($0.localizedName)'" } ?? "nil")")
      run()
    case "session":
      R(session.inputs.isEmpty, "order.session.first", "session created before discovery")
      run()
    case "threads":
      let g = DispatchGroup(); var got = [String](repeating: "-", count: 3); let lock = NSLock()
      let start = DispatchSemaphore(value: 0)
      let calls: [() -> String] = [
        { AVCaptureDevice.default(for: .video)?.uniqueID ?? "nil" },
        { "\(AVCaptureDevice.DiscoverySession(deviceTypes: allTypes, mediaType: .video, position: .unspecified).devices.count)" },
        { let s = AVCaptureSession(); return s.isRunning ? "running?" : "session" },
      ]
      var ms = [Int](repeating: -1, count: 3); var t0 = Date()
      for (i, c) in calls.enumerated() {
        DispatchQueue.global().async(group: g) { start.wait(); let r = c(); let d = Int(Date().timeIntervalSince(t0) * 1000); lock.lock(); got[i] = r; ms[i] = d; lock.unlock() }
      }
      armW4Timers(); t0 = Date()
      for _ in 0..<3 { start.signal() }
      let ok = g.wait(timeout: .now() + 15) == .success
      for i in 0..<3 { P("INFO firstcall.ms thread=\(i) ms=\(ms[i])") }
      P("INFO firstcall.ms=\(ms.max() ?? -1)")
      sigCheck()
      if expectStock { R(ok && got[0] == "nil" && got[1] == "0", "order.threads.stock", "results=\(got) finished=\(ok)"); done(); return }
      R(ok && got[0] != "nil" && got[1] != "0", "order.threads", "results=\(got) finished=\(ok)")
      run()
    default:
      R(false, "order", "unknown M5APP_ORDER=\(order)"); run()
    }
  }
}
// ---- W4 (bounded activation, the lazy-activation plan) ----
//   M5APP_EXPECT=stock       activation is expected to end stock: the camera checks become stock.* checks (no devices), then DONE
//   M5APP_SIGCHECK=1         after the first capture call returns, print the image owning the current SIGSEGV handler
//   M5APP_FAKESDK_AT_MS=<n>  n ms after the first capture call starts, a background thread installs a chaining SIGSEGV/SIGBUS
//                            handler (a stand-in for a crash SDK) that prints "FAKESDK record" once and chains to the previous one
//   M5APP_FAULT_AT_MS=<n>    n ms after the first capture call starts, a background thread faults in app code
//   M5APP_SIGCHECK_AT_MS=<n> n ms after the first capture call starts, a background thread prints "INFO sigcheck during SIGSEGV
//                            handler=<image>" (the handler while prepare still runs)
//   INFO firstcall.ms=<ms>   how long the first capture call took (per thread in the threads mode, from the shared start)
//   M5APP_EXPECT_MOTION=stock (W4 PR B) CoreMotion is expected stock (a marker): the motion checks of motion-only and
//                            camera-then-motion become order.<mode>.stock.* checks (no accelerometer, no updates), then DONE
let w4env = ProcessInfo.processInfo.environment
let expectStock = w4env["M5APP_EXPECT"] == "stock"
let expectMotionStock = w4env["M5APP_EXPECT_MOTION"] == "stock"
var gFakePrevSegv = sigaction(), gFakePrevBus = sigaction()
let fakeSDKHandler: @convention(c) (Int32, UnsafeMutablePointer<__siginfo>?, UnsafeMutableRawPointer?) -> Void = { sig, si, ctx in
  let msg: StaticString = "M5APP| FAKESDK record\n"
  _ = write(1, msg.utf8Start, msg.utf8CodeUnitCount)
  var prev = sig == SIGBUS ? gFakePrevBus : gFakePrevSegv
  if prev.sa_flags & SA_SIGINFO != 0, let f = prev.__sigaction_u.__sa_sigaction { sigaction(sig, &prev, nil); f(sig, si, ctx); return }
  signal(sig, SIG_DFL)   // return: the fault runs again under the default action
}
func sigOwner() -> String {
  var sa = sigaction(); sigaction(SIGSEGV, nil, &sa)
  let raw: UnsafeRawPointer? = sa.sa_flags & SA_SIGINFO != 0 ? unsafeBitCast(sa.__sigaction_u.__sa_sigaction, to: UnsafeRawPointer?.self) : unsafeBitCast(sa.__sigaction_u.__sa_handler, to: UnsafeRawPointer?.self)
  guard let p = raw else { return "SIG_DFL" }
  if Int(bitPattern: p) == 1 { return "SIG_IGN" }
  var info = Dl_info()
  if dladdr(p, &info) != 0, let f = info.dli_fname { return (String(cString: f) as NSString).lastPathComponent + (info.dli_sname.map { ":" + String(cString: $0) } ?? "") }
  return "?"
}
func sigCheck() { if w4env["M5APP_SIGCHECK"] == "1" { P("INFO sigcheck SIGSEGV handler=\(sigOwner())") } }
func firstCallDone(_ t0: Date) { P("INFO firstcall.ms=\(Int(Date().timeIntervalSince(t0) * 1000))"); sigCheck() }
func armW4Timers() {
  if let s = w4env["M5APP_SIGCHECK"], s == "1" { P("INFO sigcheck before SIGSEGV handler=\(sigOwner())") }
  if let n = w4env["M5APP_FAKESDK_AT_MS"].flatMap(Int.init) {
    DispatchQueue.global().asyncAfter(deadline: .now() + .milliseconds(n)) {
      var sa = sigaction(); sa.__sigaction_u.__sa_sigaction = fakeSDKHandler; sa.sa_flags = SA_SIGINFO | SA_ONSTACK
      sigaction(SIGSEGV, &sa, &gFakePrevSegv); sigaction(SIGBUS, &sa, &gFakePrevBus)
      P("INFO fakesdk installed (previous SIGSEGV handler: \(gFakePrevSegv.sa_flags & SA_SIGINFO != 0 ? "a handler" : "default"))")
    }
  }
  if let n = w4env["M5APP_SIGCHECK_AT_MS"].flatMap(Int.init) {
    DispatchQueue.global().asyncAfter(deadline: .now() + .milliseconds(n)) { P("INFO sigcheck during SIGSEGV handler=\(sigOwner())") }
  }
  if let n = w4env["M5APP_FAULT_AT_MS"].flatMap(Int.init) {
    Thread.detachNewThread { Thread.sleep(forTimeInterval: Double(n) / 1000); P("INFO app fault now"); appFault() }
  }
}
@inline(never) func appFault() { let p = UnsafeMutablePointer<Int>(bitPattern: Int(atoi("8")))!; p.pointee = 1 }
extension VC {
  // the motion checks of M5APP_ORDER=motion (one push manager at 100 Hz), for the W4 modes that don't go on to the camera
  func motionChecks(_ order: String, then next: @escaping () -> Void) {
    let hz = 100.0
    let push = CMMotionManager(); push.accelerometerUpdateInterval = 1 / hz
    let t = Date(); let avail = push.isAccelerometerAvailable
    let qms = String(format: "%.2f", Date().timeIntervalSince(t) * 1000)
    if expectMotionStock {   // stock CoreMotion in the simulator: no accelerometer, and a start delivers nothing
      R(!avail, "order.\(order).stock.available", "accel=\(avail) query.ms=\(qms)")
      motion = [push]; motionCounts = [0]
      push.startAccelerometerUpdates(to: OperationQueue()) { d, _ in if d != nil { DispatchQueue.main.async { self.motionCounts[0] += 1 } } }
      _ = push.accelerometerData; _ = push.isAccelerometerActive
      DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
        R(self.motionCounts[0] == 0, "order.\(order).stock.updates", "updates=\(self.motionCounts[0])")
        push.stopAccelerometerUpdates(); next()
      }
      return
    }
    R(avail, "order.\(order).available", "accel=\(avail) query.ms=\(qms)")
    motion = [push]; motionCounts = [0]
    let q = OperationQueue(); q.maxConcurrentOperationCount = 1
    var badTs = 0
    push.startAccelerometerUpdates(to: q) { d, _ in
      guard let d else { return }
      if abs(ProcessInfo.processInfo.systemUptime - d.timestamp) > 1.0 { badTs += 1 }
      DispatchQueue.main.async { self.motionCounts[0] += 1 }
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
      let n = self.motionCounts[0]; let want = Int(hz * 1.5)
      R(Double(n) >= 0.9 * Double(want), "order.\(order).rate", "updates=\(n) want>=\(Int(0.9 * Double(want))) badTimestamps=\(badTs)")
      R(badTs == 0, "order.\(order).timestamps", "outside systemUptime±1s: \(badTs)")
      next()
    }
  }
}
// ---- w4 e58: camera mount/unmount cycles in one process (memory) ----
//   M5APP_MODE=cycle M5APP_CYCLES=<n> (default 50), M5APP_CYCLE_WAIT=<s> (idle s seconds first), M5APP_CYCLE_HOLD=1 (no exit at
//   the end): each cycle builds a new session (back wide input, video data output,
//   photo output, preview layer), starts it, waits for 10 frames, stops it and drops everything. M5APP_CYCLE_PHOTO=1 also
//   takes one photo per cycle, M5APP_CYCLE_MOVIE=1 records a 1 s movie (movie file output). After each cycle (and 0.4 s idle, M5APP_CYCLE_IDLE=<s>): "MEM cycle=<i> fp_kb=<phys_footprint> frames=<n>".
//   Ends with "INFO cycle ok=<ok>/<n> fp0_kb=<after cycle 1> fp_end_kb=<last> slope_kb=<least-squares KB per cycle, 2..n>".
func physFootprintKB() -> Int {
  var info = task_vm_info_data_t(); var n = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size)
  let kr = withUnsafeMutablePointer(to: &info) { $0.withMemoryRebound(to: integer_t.self, capacity: Int(n)) { task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &n) } }
  return kr == KERN_SUCCESS ? Int(info.phys_footprint / 1024) : -1
}
let cyclePOI = OSLog(subsystem: "dev.simcam.spike.m10plain", category: .pointsOfInterest)
extension VC {
  func cycle() {
    let n = Int(w4env["M5APP_CYCLES"] ?? "") ?? 50; let photo = w4env["M5APP_CYCLE_PHOTO"] == "1"; let movie = w4env["M5APP_CYCLE_MOVIE"] == "1"
    P("BEGIN cycle n=\(n) photo=\(photo) movie=\(movie) fp_kb=\(physFootprintKB())")
    DispatchQueue.global().async {
      if let w = Double(w4env["M5APP_CYCLE_WAIT"] ?? "") { P("WAIT \(w)"); Thread.sleep(forTimeInterval: w) }   // time to attach a profiler
      var ok = 0; var fps: [Int] = []
      for i in 1...n {
        autoreleasepool {
          guard let back = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else { R(false, "cycle.\(i).device"); return }
          let m = M9(); let s = AVCaptureSession(); var layer: AVCaptureVideoPreviewLayer?
          let vq = DispatchQueue(label: "vdo")
          do {
            let input = try AVCaptureDeviceInput(device: back)
            s.beginConfiguration(); s.sessionPreset = .photo
            if s.canAddInput(input) { s.addInput(input) }
            let vdo = AVCaptureVideoDataOutput(); vdo.setSampleBufferDelegate(m, queue: vq); if s.canAddOutput(vdo) { s.addOutput(vdo) }
            let po = AVCapturePhotoOutput(); if s.canAddOutput(po) { s.addOutput(po) }
            let mo = AVCaptureMovieFileOutput(); if movie, s.canAddOutput(mo) { s.addOutput(mo) }
            s.commitConfiguration()
            DispatchQueue.main.sync { let l = AVCaptureVideoPreviewLayer(session: s); l.frame = self.view.bounds; self.view.layer.addSublayer(l); layer = l }
            s.startRunning()
            let t = Date(); while vq.sync(execute: { m.frames }) < 10 && Date().timeIntervalSince(t) < 5 { Thread.sleep(forTimeInterval: 0.02) }
            var bad = false
            if photo { m.photoCB = { _, _ in }; po.capturePhoto(with: AVCapturePhotoSettings(), delegate: m)
              if m.finished.wait(timeout: .now() + 3) == .timedOut { bad = true; R(false, "cycle.\(i).photo", "timeout") } }
            if movie {
              let url = FileManager.default.temporaryDirectory.appendingPathComponent("cycle.mov"); try? FileManager.default.removeItem(at: url)
              let done = DispatchSemaphore(value: 0); m.movieCB = { _, _ in done.signal() }
              mo.startRecording(to: url, recordingDelegate: m); Thread.sleep(forTimeInterval: 1.0); mo.stopRecording()
              if done.wait(timeout: .now() + 5) == .timedOut { bad = true; R(false, "cycle.\(i).movie", "timeout") }
              try? FileManager.default.removeItem(at: url)
            }
            let f = vq.sync { m.frames }; if f >= 10 && !bad { ok += 1 }   // ok: frames, and the photo/movie (if asked) arrived
            s.stopRunning()
            DispatchQueue.main.sync { layer?.session = nil; layer?.removeFromSuperlayer(); layer = nil }
            s.beginConfiguration(); for o in s.outputs { s.removeOutput(o) }; for x in s.inputs { s.removeInput(x) }; s.commitConfiguration()
            vdo.setSampleBufferDelegate(nil, queue: nil)
            if f < 10 { R(false, "cycle.\(i).frames", "n=\(f)") }
          } catch { R(false, "cycle.\(i).input", "\(error)") }
        }
        Thread.sleep(forTimeInterval: Double(w4env["M5APP_CYCLE_IDLE"] ?? "") ?? 0.4)
        let fp = physFootprintKB(); fps.append(fp); P("MEM cycle=\(i) fp_kb=\(fp)")
        os_signpost(.event, log: cyclePOI, name: "cycle", "%d", i)   // Instruments' Points of Interest: aligns cycles in a trace
      }
      let xs = Array(fps.dropFirst()); var slope = 0.0
      if xs.count >= 2 { let k = Double(xs.count); let mx = (k - 1) / 2; let my = Double(xs.reduce(0, +)) / k
        var a = 0.0, b = 0.0; for (j, y) in xs.enumerated() { a += (Double(j) - mx) * (Double(y) - my); b += (Double(j) - mx) * (Double(j) - mx) }; slope = a / b }
      P("INFO cycle ok=\(ok)/\(n) fp0_kb=\(fps.first ?? -1) fp_end_kb=\(fps.last ?? -1) slope_kb=\(String(format: "%.1f", slope))")
      if w4env["M5APP_CYCLE_HOLD"] == "1" { P("HOLD"); return }   // keep the process for heap/leaks; the driver terminates it
      DispatchQueue.main.async { self.done() }
    }
  }
}
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
