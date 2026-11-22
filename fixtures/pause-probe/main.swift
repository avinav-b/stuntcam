// SPDX-License-Identifier: Apache-2.0
// pause-probe: one AVCaptureSession driven by host commands, so the host can pause it (lldb breakpoint, background,
// kill -STOP) at a known moment and see whether every operation still completes.
// Commands: lines "<seq> <cmd> [arg]" appended to Documents/cmd.txt in the app container (polled every 20 ms):
//   setup [4k60]   session: back wide camera, VDO, photo output (max photo dims), movie file output, preview; startRunning
//   photo <n>      a burst of n photos (max dims, JPEG). Logs PHOTO-REQ, PHOTO-PROC (didFinishProcessingPhoto), PHOTO-DONE
//   focus          focus point + autoFocus: KVO on isAdjustingFocus fires
//   rec start|stop movie file output to Documents/mov-<k>.mov; MOVIE-DONE logs error, duration and video sample count
//   stop | start   stopRunning / startRunning on the session queue (CALL/RET with ms)
//   armresign <n>  take a burst of n photos inside the next sceneWillResignActive (so they are in flight as the app backgrounds)
//   armbg <n>      the same inside the next sceneDidEnterBackground (just before the engine's background interruption)
//   status         one STATUS line; exit: exit(0)
// Breakpoint targets (plain C symbols): probe_photo_hit (photo delegate), probe_kvo_hit (isAdjustingFocus KVO),
// probe_kvo_running_hit (session isRunning KVO), probe_vdo_hit (video data output delegate, every frame).
// Every line: "PP| <CFAbsoluteTime> <s since launch> <text>" (CFAbsoluteTime = the engine log's clock).
import UIKit
import AVFoundation

let T0 = CFAbsoluteTimeGetCurrent()
let logLock = NSLock()
func P(_ s: String) {
  let t = CFAbsoluteTimeGetCurrent()
  logLock.lock(); print(String(format: "PP| %.3f %7.3f ", t, t - T0) + s); fflush(stdout); logLock.unlock()
}

@_cdecl("probe_photo_hit") @inline(never) public func probe_photo_hit(_ n: Int32) { P("HIT photo \(n)") }
@_cdecl("probe_kvo_hit") @inline(never) public func probe_kvo_hit(_ v: Int32) { P("HIT kvo isAdjustingFocus=\(v)") }
@_cdecl("probe_kvo_running_hit") @inline(never) public func probe_kvo_running_hit(_ v: Int32) { P("HIT kvo isRunning=\(v)") }
var vdoHits = 0
@_cdecl("probe_vdo_hit") @inline(never) public func probe_vdo_hit(_ n: Int32) { vdoHits &+= 1 }

final class Probe: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate {
  let session = AVCaptureSession()
  let q = DispatchQueue(label: "probe.session")
  let vdoQ = DispatchQueue(label: "probe.vdo")
  let photo = AVCapturePhotoOutput(), movie = AVCaptureMovieFileOutput(), vdo = AVCaptureVideoDataOutput()
  var device: AVCaptureDevice?
  var obs: [NSKeyValueObservation] = []
  var frames = 0, lastFrames = 0, photoN = 0, movN = 0
  var reqToN: [Int64: Int] = [:]
  weak var view: UIView?

  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) { frames += 1; probe_vdo_hit(Int32(frames)) }
  func captureOutput(_ o: AVCaptureOutput, didDrop sb: CMSampleBuffer, from c: AVCaptureConnection) { P("VDO dropped") }

  func setup(_ arg: String?) {
    guard let d = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else { P("SETUP-FAIL no device"); return }
    device = d
    do {
      let input = try AVCaptureDeviceInput(device: d)
      session.beginConfiguration()
      if session.canAddInput(input) { session.addInput(input) }
      vdo.setSampleBufferDelegate(self, queue: vdoQ); if session.canAddOutput(vdo) { session.addOutput(vdo) }
      if session.canAddOutput(photo) { session.addOutput(photo) }
      if session.canAddOutput(movie) { session.addOutput(movie) }
      if arg == "4k60" {
        session.sessionPreset = .inputPriority
        let f = d.formats.first { let x = CMVideoFormatDescriptionGetDimensions($0.formatDescription); return x.width == 3840 && x.height == 2160 && $0.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 60 } }
        if let f {
          try d.lockForConfiguration(); d.activeFormat = f
          d.activeVideoMinFrameDuration = CMTime(value: 1, timescale: 60); d.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: 60)
          d.unlockForConfiguration()
          P("FORMAT 4k60 \(f)")
        } else { P("FORMAT-FAIL no 3840x2160@60 format; formats=\(d.formats.count)") }
      } else { session.sessionPreset = .photo }
      session.commitConfiguration()
      if let m = d.activeFormat.supportedMaxPhotoDimensions.max(by: { Int($0.width) * Int($0.height) < Int($1.width) * Int($1.height) }) { photo.maxPhotoDimensions = m }
      let a = d.activeFormat, dims = CMVideoFormatDescriptionGetDimensions(a.formatDescription)
      P("SETUP outputs=\(session.outputs.count) active=\(dims.width)x\(dims.height) fps=\(1 / CMTimeGetSeconds(d.activeVideoMinFrameDuration)) photoMax=\(photo.maxPhotoDimensions.width)x\(photo.maxPhotoDimensions.height)")
    } catch { P("SETUP-FAIL \(error)"); return }
    obs.append(d.observe(\.isAdjustingFocus, options: [.new]) { _, ch in let v = ch.newValue ?? false; probe_kvo_hit(v ? 1 : 0) })
    obs.append(session.observe(\.isRunning, options: [.new]) { _, ch in let v = ch.newValue ?? false; probe_kvo_running_hit(v ? 1 : 0) })
    let nc = NotificationCenter.default
    for (n, label) in [(AVCaptureSession.didStartRunningNotification, "DidStartRunning"), (.AVCaptureSessionDidStopRunning, "DidStopRunning"),
                       (.AVCaptureSessionWasInterrupted, "WasInterrupted"), (.AVCaptureSessionInterruptionEnded, "InterruptionEnded"),
                       (.AVCaptureSessionRuntimeError, "RuntimeError")] {
      nc.addObserver(forName: n, object: session, queue: nil) { [weak self] note in
        P("NOTE \(label) running=\(self?.session.isRunning ?? false) interrupted=\(self?.session.isInterrupted ?? false) info=\(note.userInfo.map { "\($0)" } ?? "-")")
      }
    }
    DispatchQueue.main.async {
      if let v = self.view { let l = AVCaptureVideoPreviewLayer(session: self.session); l.frame = v.bounds; l.videoGravity = .resizeAspectFill; v.layer.addSublayer(l) }
    }
    call("start") { self.session.startRunning() }
  }
  func call(_ name: String, _ f: () -> Void) {
    P("CALL \(name) running=\(session.isRunning) interrupted=\(session.isInterrupted)")
    let t = CFAbsoluteTimeGetCurrent(); f()
    P(String(format: "RET %@ %.0f ms running=%d interrupted=%d", name, (CFAbsoluteTimeGetCurrent() - t) * 1000, session.isRunning ? 1 : 0, session.isInterrupted ? 1 : 0))
  }
  func takePhotos(_ n: Int) {
    for _ in 0..<n {
      photoN += 1
      let s = AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg])
      s.maxPhotoDimensions = photo.maxPhotoDimensions
      reqToN[s.uniqueID] = photoN
      P("PHOTO-REQ \(photoN) id=\(s.uniqueID)")
      photo.capturePhoto(with: s, delegate: self)
    }
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishProcessingPhoto p: AVCapturePhoto, error: Error?) {
    let n = q.sync { reqToN[p.resolvedSettings.uniqueID] ?? -1 }
    probe_photo_hit(Int32(n))
    P("PHOTO-PROC \(n) bytes=\(p.fileDataRepresentation()?.count ?? 0) err=\(error.map { "\($0)" } ?? "nil")")
  }
  func photoOutput(_ o: AVCapturePhotoOutput, didFinishCaptureFor r: AVCaptureResolvedPhotoSettings, error: Error?) {
    let n = q.sync { reqToN[r.uniqueID] ?? -1 }
    P("PHOTO-DONE \(n) err=\(error.map { "\($0)" } ?? "nil")")
  }
  func record(_ arg: String?) {
    if arg == "stop" { P("REC-STOP-CALL recording=\(movie.isRecording)"); movie.stopRecording(); return }
    movN += 1
    let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0], u = docs.appendingPathComponent("mov-\(movN).mov")
    try? FileManager.default.removeItem(at: u)
    P("REC-START \(movN) \(u.path)")
    movie.startRecording(to: u, recordingDelegate: self)
  }
  func fileOutput(_ o: AVCaptureFileOutput, didStartRecordingTo u: URL, from c: [AVCaptureConnection]) { P("MOVIE-STARTED \(u.lastPathComponent)") }
  func fileOutput(_ o: AVCaptureFileOutput, didFinishRecordingTo u: URL, from c: [AVCaptureConnection], error: Error?) {
    let ok = (error as NSError?)?.userInfo[AVErrorRecordingSuccessfullyFinishedKey] as? Bool
    P("MOVIE-FINISH-CB \(u.lastPathComponent) err=\(error.map { ($0 as NSError).code.description + " " + $0.localizedDescription } ?? "nil") successfullyFinished=\(ok.map { "\($0)" } ?? "-")")
    DispatchQueue.global().async {
      let a = AVURLAsset(url: u); var dur = -1.0, samples = -1, fps: Float = -1, dims = "-"
      if let r = try? AVAssetReader(asset: a), let t = a.tracks(withMediaType: .video).first {
        dur = CMTimeGetSeconds(a.duration); fps = t.nominalFrameRate; dims = "\(Int(t.naturalSize.width))x\(Int(t.naturalSize.height))"
        let out = AVAssetReaderTrackOutput(track: t, outputSettings: nil); r.add(out); r.startReading(); samples = 0
        while let sb = out.copyNextSampleBuffer() { if CMSampleBufferGetNumSamples(sb) > 0 { samples += 1 } }
      }
      let size = (try? FileManager.default.attributesOfItem(atPath: u.path)[.size] as? Int) ?? -1
      P("MOVIE-DONE \(u.lastPathComponent) bytes=\(size) dur=\(String(format: "%.2f", dur)) videoSamples=\(samples) nominalFps=\(fps) dims=\(dims) readable=\(samples > 0)")
    }
  }
  func focus() {
    guard let d = device else { return }
    do { try d.lockForConfiguration()
      if d.isFocusPointOfInterestSupported { d.focusPointOfInterest = CGPoint(x: Double.random(in: 0.3...0.7), y: 0.5) }
      if d.isFocusModeSupported(.autoFocus) { d.focusMode = .autoFocus }
      d.unlockForConfiguration(); P("FOCUS requested")
    } catch { P("FOCUS-FAIL \(error)") }
  }
  func run(_ cmd: String, _ arg: String?) {
    switch cmd {
    case "setup": setup(arg)
    case "photo": takePhotos(Int(arg ?? "1") ?? 1)
    case "focus": focus()
    case "rec": record(arg)
    case "stop": call("stop") { session.stopRunning() }
    case "start": call("start") { session.startRunning() }
    case "armresign": armResign = Int(arg ?? "1") ?? 1; P("ARMED resign burst \(armResign)")
    case "armbg": armBg = Int(arg ?? "1") ?? 1; P("ARMED background burst \(armBg)")
    case "status": P("STATUS running=\(session.isRunning) interrupted=\(session.isInterrupted) frames=\(frames) recording=\(movie.isRecording) photos=\(photoN) movies=\(movN) vdoHits=\(vdoHits) appState=\(DispatchQueue.main.sync { UIApplication.shared.applicationState.rawValue })")
    case "exit": P("EXIT"); exit(0)
    default: P("UNKNOWN \(cmd)")
    }
  }
}

let probe = Probe()
var armResign = 0, armBg = 0
var lastSeq = 0
func startCommandPoll() {
  let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
  let path = docs.appendingPathComponent("cmd.txt").path
  P("READY pid=\(getpid()) cmd=\(path)")
  let t = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "probe.poll"))
  t.schedule(deadline: .now() + 0.1, repeating: 0.02)
  t.setEventHandler {
    guard let s = try? String(contentsOfFile: path, encoding: .utf8) else { return }
    for line in s.split(separator: "\n") {
      let f = line.split(separator: " ").map(String.init); guard f.count >= 2, let seq = Int(f[0]), seq > lastSeq else { continue }
      lastSeq = seq; P("CMD \(seq) \(f[1]) \(f.count > 2 ? f[2] : "")")
      probe.q.async { probe.run(f[1], f.count > 2 ? f[2] : nil) }
    }
  }
  t.resume()
  // frames per second, logged every second (a stopped process logs nothing, so gaps show in the log)
  let ft = DispatchSource.makeTimerSource(queue: DispatchQueue(label: "probe.fps"))
  ft.schedule(deadline: .now() + 1, repeating: 1)
  ft.setEventHandler { let f = probe.frames; P("FPS \(f - probe.lastFrames) total=\(f)"); probe.lastFrames = f }
  ft.resume()
  objc_setAssociatedObject(probe, "timers", [t, ft], .OBJC_ASSOCIATION_RETAIN)
}

final class VC: UIViewController {
  override func viewDidLoad() { super.viewDidLoad(); view.backgroundColor = .black; probe.view = view }
}
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo s: UISceneSession, options: UIScene.ConnectionOptions) {
    guard let ws = scene as? UIWindowScene else { return }
    let w = UIWindow(windowScene: ws); w.rootViewController = VC(); w.makeKeyAndVisible(); window = w
  }
  func sceneWillResignActive(_ s: UIScene) {
    P("SCENE willResignActive")
    let n = probe.q.sync { () -> Int in let n = armResign; armResign = 0; if n > 0 { probe.takePhotos(n) }; return n }
    if n > 0 { P("RESIGN burst of \(n) requested") }
  }
  func sceneDidEnterBackground(_ s: UIScene) {
    P("SCENE didEnterBackground")
    let n = probe.q.sync { () -> Int in let n = armBg; armBg = 0; if n > 0 { probe.takePhotos(n) }; return n }
    if n > 0 { P("BACKGROUND burst of \(n) requested") }
  }
  func sceneWillEnterForeground(_ s: UIScene) { P("SCENE willEnterForeground") }
  func sceneDidBecomeActive(_ s: UIScene) { P("SCENE didBecomeActive") }
}
final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]? = nil) -> Bool { startCommandPoll(); return true }
  func application(_ a: UIApplication, configurationForConnecting s: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: nil, sessionRole: s.role); c.delegateClass = SceneDelegate.self; return c
  }
}
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
