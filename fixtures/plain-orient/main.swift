// SPDX-License-Identifier: Apache-2.0
// m12 plain AVFoundation test app. Mode via env P12_MODE = orient | lifecycle | stress ; P12_CAM = back | front ; P12_N = cycles
import UIKit
import AVFoundation
import CoreMotion

func P(_ s: String) { let l = "P12| " + s; print(l); fflush(stdout); LogFile.write(l) }
enum LogFile {
  static let url = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0].appendingPathComponent("p12.log")
  static let q = DispatchQueue(label: "log")
  static func write(_ s: String) { q.sync { if let h = try? FileHandle(forWritingTo: url) { h.seekToEndOfFile(); h.write((s + "\n").data(using: .utf8)!); try? h.close() } else { try? (s + "\n").write(to: url, atomically: false, encoding: .utf8) } } }
}
let env = ProcessInfo.processInfo.environment
let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]

final class VC: UIViewController, AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate, AVCaptureVideoDataOutputSampleBufferDelegate {
  lazy var session = AVCaptureSession()   // W4: lazy, so the coord-* modes make the first capture call themselves
  lazy var photo = AVCapturePhotoOutput()        // W4: lazy too: creating either output makes AVFCapture itself call
  lazy var movie = AVCaptureMovieFileOutput()    // +[AVCaptureDeviceDiscoverySession …] (a trapped entry point) on main
  var layer: AVCaptureVideoPreviewLayer!
  var coord: AVCaptureDevice.RotationCoordinator?
  var obs: [NSKeyValueObservation] = []
  var photoDone: ((Data?, Error?) -> Void)?
  var movieDone: ((URL, Error?) -> Void)?
  // W4: P12_UI=landscape -> a landscape-only UI; P12_UI=turnback -> the same, then portrait-only once the UI is landscape
  // (coord-bg only: the UI turns twice before the first camera call)
  var uiMask: UIInterfaceOrientationMask = ["landscape", "turnback"].contains(env["P12_UI"] ?? "") ? .landscapeRight : .allButUpsideDown
  override var supportedInterfaceOrientations: UIInterfaceOrientationMask { uiMask }

  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    let mode = env["P12_MODE"] ?? "orient"
    P("BEGIN mode=\(mode) cam=\(env["P12_CAM"] ?? "back") ui=\(view.window!.windowScene!.effectiveGeometry.interfaceOrientation.rawValue) dev=\(UIDevice.current.orientation.rawValue)")
    // W4 PR B: P12_MOTION_QUERY=1 asks CoreMotion one availability question first (no updates, so no motion tracking), so a
    // CoreMotion-stock launch prints the engine's stock line (orient-nomotion-marker)
    if env["P12_MOTION_QUERY"] == "1" { let m = CMMotionManager(); P("INFO motion accel=\(m.isAccelerometerAvailable)") }
    if mode == "coord-first" { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.coordFirst() }; return }
    if mode == "coord-bg" && env["P12_UI"] == "turnback" { turnBack { self.coordBg() }; return }
    if mode == "coord-bg" { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.coordBg() }; return }
    DispatchQueue.global().async { mode == "stress" ? self.stress() : mode == "lifecycle" ? self.lifecycle() : self.orient() }
  }
  // ---- W4 (the lazy-activation plan): the RotationCoordinator built right at the first capture call
  //   coord-first  on main, in the same run-loop turn as the first capture call (use with P12_UI=landscape)
  //   coord-bg     the first capture call and the coordinator on a serial camera queue, while main is busy for 300 ms
  // Each prints "COORD <when> preview=<a> capture=<a> ui=<o> dev=<o>" right away and again 1.5 s later, and PASS/FAIL coord-<mode>:
  // landscape angles (not 90) right away. coord-bg with P12_UI=turnback prints PASS/FAIL coord-bg-turnback instead: the UI is
  // portrait again, so the preview angle must be 90 right away.
  // P12_UI=turnback: wait until the landscape-only UI is landscape (3), then allow portrait only and wait until it is portrait
  // (1) again, then run the mode 0.3 s later. Two geometry changes, both before the first camera call.
  func turnBack(_ then: @escaping () -> Void) {
    let ws = view.window!.windowScene!, t0 = Date()
    func ms() -> Int { Int(Date().timeIntervalSince(t0) * 1000) }
    func wait(_ want: Int, _ next: @escaping () -> Void) {
      if ws.effectiveGeometry.interfaceOrientation.rawValue == want { next(); return }
      if ms() > 5000 { P("FAIL coord-bg-turnback ui stuck at \(ws.effectiveGeometry.interfaceOrientation.rawValue), wanted \(want)"); P("DONE"); exit(0) }
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { wait(want, next) }
    }
    wait(3) {
      P("INFO turnback ui=3 at \(ms()) ms; now portrait only")
      self.uiMask = .portrait; self.setNeedsUpdateOfSupportedInterfaceOrientations()
      ws.requestGeometryUpdate(.iOS(interfaceOrientations: .portrait)) { e in P("INFO turnback requestGeometryUpdate error \(e)") }
      wait(1) { P("INFO turnback ui=1 at \(ms()) ms"); DispatchQueue.main.asyncAfter(deadline: .now() + 0.3, execute: then) }
    }
  }
  func coordReport(_ tag: String, _ c: AVCaptureDevice.RotationCoordinator, ui: Int) -> (CGFloat, CGFloat) {
    let p = c.videoRotationAngleForHorizonLevelPreview, cap = c.videoRotationAngleForHorizonLevelCapture
    P("COORD \(tag) preview=\(p) capture=\(cap) ui=\(ui) dev=\(UIDevice.current.orientation.rawValue)"); return (p, cap)
  }
  func coordFirst() {
    let t0 = Date()
    let d = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
    P("INFO firstcall.ms=\(Int(Date().timeIntervalSince(t0) * 1000))")
    guard let d else { P("FAIL coord-first no device"); P("DONE"); exit(0) }
    let c = AVCaptureDevice.RotationCoordinator(device: d, previewLayer: AVCaptureVideoPreviewLayer())
    let ui = view.window?.windowScene?.effectiveGeometry.interfaceOrientation.rawValue ?? -1
    let (p, cap) = coordReport("same-turn", c, ui: ui)
    P("\(p != 90 && cap != 90 ? "PASS" : "FAIL") coord-first preview=\(p) capture=\(cap) (same run-loop turn as the first capture call)")
    DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { _ = self.coordReport("later", c, ui: ui); P("DONE"); exit(0) }
  }
  func coordBg() {
    let ui = view.window?.windowScene?.effectiveGeometry.interfaceOrientation.rawValue ?? -1
    let camQ = DispatchQueue(label: "camera.queue")
    camQ.async {
      let t0 = Date()
      let d = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back)
      P("INFO firstcall.ms=\(Int(Date().timeIntervalSince(t0) * 1000))")
      guard let d else { P("FAIL coord-bg no device"); P("DONE"); exit(0) }
      let c = AVCaptureDevice.RotationCoordinator(device: d, previewLayer: nil)
      let (p, cap) = self.coordReport("camera-queue", c, ui: ui)
      if env["P12_UI"] == "turnback" { P("\(p == 90 ? "PASS" : "FAIL") coord-bg-turnback preview=\(p) capture=\(cap) ui=\(ui) (camera queue, main busy; UI turned 3 then 1 before)") }
      else { P("\(p != 90 && cap != 90 ? "PASS" : "FAIL") coord-bg preview=\(p) capture=\(cap) (camera queue, main busy)") }
      Thread.sleep(forTimeInterval: 1.5); _ = self.coordReport("later", c, ui: ui); P("DONE"); exit(0)
    }
    let tb = Date(); while Date().timeIntervalSince(tb) < 0.3 { }   // main busy for 300 ms
    P("INFO main free again")
  }
  func device(_ cam: String) -> AVCaptureDevice { AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: cam == "front" ? .front : .back)! }
  func waitFile(_ name: String, _ secs: Double) { let u = docs.appendingPathComponent(name); let t = Date(); while Date().timeIntervalSince(t) < secs { if FileManager.default.fileExists(atPath: u.path) { try? FileManager.default.removeItem(at: u); return }; usleep(100_000) } ; P("INFO no \(name) after \(secs)s") }

  func setup(_ cam: String, audio: Bool = false) {
    let d = device(cam)
    session.beginConfiguration()
    session.addInput(try! AVCaptureDeviceInput(device: d))
    if audio, let mic = AVCaptureDevice.default(for: .audio), let mi = try? AVCaptureDeviceInput(device: mic), session.canAddInput(mi) { session.addInput(mi) }
    session.addOutput(photo); session.addOutput(movie)
    session.commitConfiguration()
    DispatchQueue.main.sync {
      layer = AVCaptureVideoPreviewLayer(session: session); layer.frame = view.bounds; layer.videoGravity = .resizeAspectFill
      view.layer.addSublayer(layer)
      let c = AVCaptureDevice.RotationCoordinator(device: d, previewLayer: layer); coord = c
      layer.connection?.videoRotationAngle = c.videoRotationAngleForHorizonLevelPreview
      obs.append(c.observe(\.videoRotationAngleForHorizonLevelPreview, options: [.new]) { [weak self] c, _ in DispatchQueue.main.async { self?.layer.connection?.videoRotationAngle = c.videoRotationAngleForHorizonLevelPreview; self?.layer.frame = self?.view.bounds ?? .zero } })
    }
  }
  func capAngle() -> CGFloat { coord?.videoRotationAngleForHorizonLevelCapture ?? 90 }

  // ---------------------------------------------------------------- Q1
  func orient() {
    let cam = env["P12_CAM"] ?? "back"
    setup(cam)
    session.startRunning()
    P("running=\(session.isRunning) previewAngle=\(coord!.videoRotationAngleForHorizonLevelPreview) captureAngle=\(capAngle()) previewMirrored=\(layer.connection?.isVideoMirrored ?? false)")
    sleep(UInt32(env["P12_HOLD"] ?? "2") ?? 2)
    DispatchQueue.main.sync { layer.frame = view.bounds }
    usleep(500_000)
    P("SCREENSHOT preview"); waitFile("shot.ack", 8)
    // photo
    let pc = photo.connection(with: .video)!; pc.videoRotationAngle = capAngle(); if env["P12_MIRROR"] == "1" { pc.automaticallyAdjustsVideoMirroring = false; pc.isVideoMirrored = true }
    P("photo connection angle=\(pc.videoRotationAngle) mirrored=\(pc.isVideoMirrored)")
    let sem = DispatchSemaphore(value: 0)
    photoDone = { data, err in
      if let data { let u = docs.appendingPathComponent("photo.jpg"); try! data.write(to: u); P("PHOTO path=\(u.path) bytes=\(data.count)") } else { P("FAIL photo \(String(describing: err))") }
      sem.signal() }
    photo.capturePhoto(with: AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg]), delegate: self)
    if sem.wait(timeout: .now() + 10) == .timedOut { P("FAIL photo timeout") }
    // movie
    let mc = movie.connection(with: .video)!; mc.videoRotationAngle = capAngle(); if env["P12_MIRROR"] == "1" { mc.automaticallyAdjustsVideoMirroring = false; mc.isVideoMirrored = true }
    P("movie connection angle=\(mc.videoRotationAngle) mirrored=\(mc.isVideoMirrored)")
    let mu = docs.appendingPathComponent("movie.mov"); try? FileManager.default.removeItem(at: mu)
    let ms = DispatchSemaphore(value: 0)
    movieDone = { u, err in P("MOVIE path=\(u.path) err=\(String(describing: err))"); ms.signal() }
    movie.startRecording(to: mu, recordingDelegate: self)
    sleep(2); movie.stopRecording()
    if ms.wait(timeout: .now() + 10) == .timedOut { P("FAIL movie timeout") }
    session.stopRunning()
    P("DONE"); usleep(300_000); exit(0)
  }
  func photoOutput(_ output: AVCapturePhotoOutput, didFinishProcessingPhoto photo: AVCapturePhoto, error: Error?) { photoDone?(photo.fileDataRepresentation(), error) }
  func fileOutput(_ output: AVCaptureFileOutput, didStartRecordingTo fileURL: URL, from connections: [AVCaptureConnection]) { P("EVENT movie didStart") }
  func fileOutput(_ output: AVCaptureFileOutput, didFinishRecordingTo outputFileURL: URL, from connections: [AVCaptureConnection], error: Error?) { movieDone?(outputFileURL, error) }

  // ---------------------------------------------------------------- Q2
  var frames = 0
  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) { frames += 1 }
  func lifecycle() {
    let nc = NotificationCenter.default
    nc.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: nil, queue: nil) { n in
      P("EVENT wasInterrupted reason=\((n.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int).map(String.init) ?? "nil") isRunning=\(self.session.isRunning) isInterrupted=\(self.session.isInterrupted)") }
    nc.addObserver(forName: AVCaptureSession.interruptionEndedNotification, object: nil, queue: nil) { _ in P("EVENT interruptionEnded isRunning=\(self.session.isRunning) isInterrupted=\(self.session.isInterrupted)") }
    nc.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: nil, queue: nil) { n in
      let e = n.userInfo?[AVCaptureSessionErrorKey] as? NSError; P("EVENT runtimeError code=\(e?.code ?? 0) domain=\(e?.domain ?? "") isRunning=\(self.session.isRunning)") }
    nc.addObserver(forName: AVCaptureSession.didStartRunningNotification, object: nil, queue: nil) { _ in P("EVENT didStartRunning") }
    nc.addObserver(forName: AVCaptureSession.didStopRunningNotification, object: nil, queue: nil) { _ in P("EVENT didStopRunning") }
    nc.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil) { _ in P("EVENT app didEnterBackground frames=\(self.frames)") }
    nc.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { _ in P("EVENT app willEnterForeground frames=\(self.frames)") }
    setup(env["P12_CAM"] ?? "back", audio: env["P12_AUDIO"] == "1")
    let vdo = AVCaptureVideoDataOutput(); vdo.setSampleBufferDelegate(self, queue: DispatchQueue(label: "vdo"))
    session.beginConfiguration(); session.addOutput(vdo); session.commitConfiguration()
    obs.append(session.observe(\.isRunning, options: [.new]) { _, ch in P("KVO isRunning -> \(ch.newValue ?? false)") })
    session.startRunning()
    P("running=\(session.isRunning)")
    // 1) record, then the host backgrounds us; 2) after return: record again and let the host send control events
    var round = 0
    while round < (Int(env["P12_ROUNDS"] ?? "3") ?? 3) {
      round += 1
      let mu = docs.appendingPathComponent("lc\(round).mov"); try? FileManager.default.removeItem(at: mu)
      let ms = DispatchSemaphore(value: 0)
      movieDone = { u, err in
        let e = err as NSError?
        let dur = CMTimeGetSeconds(AVURLAsset(url: u).duration)
        P("MOVIE round=\(round) code=\(e?.code ?? 0) domain=\(e?.domain ?? "-") successfullyFinished=\(String(describing: e?.userInfo[AVErrorRecordingSuccessfullyFinishedKey])) duration=\(String(format: "%.2f", dur)) frames=\(self.frames)")
        ms.signal() }
      while !session.isRunning { usleep(100_000) }
      movie.startRecording(to: mu, recordingDelegate: self)
      P("RECORDING round=\(round)")   // host acts now
      if ms.wait(timeout: .now() + 25) == .timedOut { P("INFO round \(round): recording not ended by an interruption in 25 s -> stop"); movie.stopRecording(); _ = ms.wait(timeout: .now() + 5) }
      let f0 = frames; sleep(3); P("AFTER round=\(round) isRunning=\(session.isRunning) isInterrupted=\(session.isInterrupted) framesIn3s=\(frames - f0)")
    }
    P("DONE"); usleep(300_000); exit(0)
  }

  // ---------------------------------------------------------------- Q3
  func stress() {
    let n = Int(env["P12_N"] ?? "200") ?? 200
    let presets: [AVCaptureSession.Preset] = [.high, .photo, .hd1280x720, .vga640x480, .hd1920x1080]
    var firstFrameFails = 0
    let t0 = Date()
    for i in 0..<n {
      autoreleasepool {
        let s = AVCaptureSession()
        let cam = i % 2 == 0 ? "back" : "front"
        let d = device(cam)
        s.beginConfiguration()
        s.sessionPreset = presets[i % presets.count]
        s.addInput(try! AVCaptureDeviceInput(device: d))
        let vdo = AVCaptureVideoDataOutput()
        let fq = DispatchQueue(label: "f\(i)")
        let got = DispatchSemaphore(value: 0)
        let del = FrameDel { got.signal() }
        vdo.setSampleBufferDelegate(del, queue: fq)
        s.addOutput(vdo)
        let ph = AVCapturePhotoOutput(); if s.canAddOutput(ph) { s.addOutput(ph) }
        s.commitConfiguration()
        s.startRunning()
        let waitFrame = i % 4 != 3   // every 4th: tear down immediately without waiting for a frame
        if waitFrame && got.wait(timeout: .now() + 3) == .timedOut { firstFrameFails += 1; P("INFO cycle \(i) no frame in 3s") }
        if i % 5 == 2 {   // switch camera and format mid-run
          s.beginConfiguration()
          for inp in s.inputs { s.removeInput(inp) }
          s.addInput(try! AVCaptureDeviceInput(device: device(cam == "back" ? "front" : "back")))
          s.sessionPreset = presets[(i + 2) % presets.count]
          s.commitConfiguration()
          usleep(UInt32.random(in: 0...60_000))
        }
        if i % 7 == 5 { DispatchQueue.global().async { s.stopRunning() }; usleep(UInt32.random(in: 0...20_000)) } else { s.stopRunning() }
        vdo.setSampleBufferDelegate(nil, queue: nil)
        _ = del
      }
      if i % 25 == 0 { P("cycle \(i) elapsed=\(String(format: "%.1f", Date().timeIntervalSince(t0)))s") }
    }
    P("STRESS cycles=\(n) noFrame=\(firstFrameFails) elapsed=\(String(format: "%.1f", Date().timeIntervalSince(t0)))s")
    sleep(1)
    P("DONE"); usleep(300_000); exit(0)
  }
}
final class FrameDel: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  let cb: () -> Void; var n = 0
  init(_ cb: @escaping () -> Void) { self.cb = cb }
  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) { n += 1; if n == 1 { cb() } }
}

final class SD: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
    let w = UIWindow(windowScene: scene as! UIWindowScene); w.rootViewController = VC(); w.makeKeyAndVisible(); window = w
  }
}
final class AD: UIResponder, UIApplicationDelegate {
  func application(_ application: UIApplication, configurationForConnecting s: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: nil, sessionRole: s.role); c.delegateClass = SD.self; return c
  }
}
try? FileManager.default.removeItem(at: LogFile.url)
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AD.self))
