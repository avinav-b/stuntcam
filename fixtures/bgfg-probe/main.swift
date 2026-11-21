// SPDX-License-Identifier: Apache-2.0
// bgfg-probe: one AVCaptureSession (back wide camera + video data output) across background and foreground.
// Measurement only: it logs, it doesn't check. Every line is
//   PROBE| <CFAbsoluteTime, 3 dp> <s since launch, monotonic> <text>
// CFAbsoluteTime is the engine log's clock (engine/src/simcam.m LOG), so the two logs merge by time.
// Logged: UIScene callbacks (and the UIApplication notifications the engine observes), every AVCaptureSession notification
// with its userInfo, isRunning / isInterrupted after each of the probe's own calls and each notification (and KVO changes of
// both), and frames per 250 ms window ("frames N" only when N changes from the previous window, plus every 1 s while > 0).
// Behaviour: launch argument `-mode stopInBackground|keepRunning|stopWhileInterrupted|appNotif` (see README.md).
// Host protocol: 3 s after the first didBecomeActive the probe prints BG-REQUEST; the host backgrounds it and brings it back.
// The probe prints END when its script is done; the host then terminates it.
import UIKit
import AVFoundation

let T0 = ProcessInfo.processInfo.systemUptime
let logLock = NSLock()
setvbuf(stdout, nil, _IOLBF, 0)
func log(_ s: String) {
  logLock.lock(); defer { logLock.unlock() }
  print(String(format: "PROBE| %.3f %7.3f ", CFAbsoluteTimeGetCurrent(), ProcessInfo.processInfo.systemUptime - T0) + s); fflush(stdout)
}
let mode: String = {
  let a = CommandLine.arguments
  if let i = a.firstIndex(of: "-mode"), i + 1 < a.count { return a[i + 1] }
  return "keepRunning"
}()

final class Probe: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  let session = AVCaptureSession()
  let sessionQ = DispatchQueue(label: "probe.session")      // the app's own session queue (Apple's pattern)
  let frameQ = DispatchQueue(label: "probe.frames")
  let tickQ = DispatchQueue(label: "probe.tick")
  var frames = 0, lastWindow = -1, windows = 0             // frameQ / tickQ via lock
  let fLock = NSLock()
  var kvo: [NSKeyValueObservation] = []
  var bgTask: UIBackgroundTaskIdentifier = .invalid
  var interruptedAt: Double? = nil
  var activeCount = 0
  var stopDoneInBg = false

  func st() -> String { "isRunning=\(session.isRunning ? 1 : 0) isInterrupted=\(session.isInterrupted ? 1 : 0)" }

  func setup() {
    log("pid=\(getpid()) mode=\(mode) os=\(UIDevice.current.systemVersion)")
    for n in [AVCaptureSession.didStartRunningNotification, AVCaptureSession.didStopRunningNotification,
              AVCaptureSession.wasInterruptedNotification, AVCaptureSession.interruptionEndedNotification,
              AVCaptureSession.runtimeErrorNotification] {
      NotificationCenter.default.addObserver(forName: n, object: session, queue: nil) { [unowned self] note in
        var extra = ""
        if let r = note.userInfo?[AVCaptureSessionInterruptionReasonKey] as? Int { extra += " reason=\(r)" }
        if let e = note.userInfo?[AVCaptureSessionErrorKey] as? NSError { extra += " error=\(e.domain) \(e.code) \"\(e.localizedDescription)\"" }
        if let u = note.userInfo, extra.isEmpty, !u.isEmpty { extra += " userInfo=\(u)" }
        log("NOTE \(n.rawValue.replacingOccurrences(of: "AVCaptureSession", with: ""))\(extra) | \(self.st()) thread=\(Thread.isMainThread ? "main" : "bg")")
        if n == AVCaptureSession.wasInterruptedNotification { self.interrupted() }
      }
    }
    kvo.append(session.observe(\.isRunning, options: [.new]) { s, c in log("KVO isRunning -> \(c.newValue.map { $0 ? 1 : 0 } ?? -1)") })
    kvo.append(session.observe(\.isInterrupted, options: [.new]) { s, c in log("KVO isInterrupted -> \(c.newValue.map { $0 ? 1 : 0 } ?? -1)") })
    sessionQ.async { [self] in
      session.beginConfiguration()
      if let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) {
        do { let inp = try AVCaptureDeviceInput(device: dev); if session.canAddInput(inp) { session.addInput(inp); log("input added: \(dev.localizedName) (\(type(of: dev)))") } else { log("canAddInput false") } }
        catch { log("device input error: \(error)") }
      } else { log("no back wide camera (AVCaptureDevice.default returned nil)") }
      let out = AVCaptureVideoDataOutput(); out.setSampleBufferDelegate(self, queue: frameQ)
      if session.canAddOutput(out) { session.addOutput(out); log("video data output added") } else { log("canAddOutput false") }
      session.commitConfiguration()
      log("configured | \(st())")
    }
    if mode == "appNotif" {
      // extra (not in the brief): stop/start from UIApplication notification observers instead of the scene callbacks, as
      // app-level code and plugins do; registered after the engine's own observers (the engine's run at load)
      NotificationCenter.default.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil) { [unowned self] _ in stop("UIApplication.didEnterBackground observer") }
      NotificationCenter.default.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { [unowned self] _ in start("UIApplication.willEnterForeground observer") }
    }
    start("launch")
    // frames per 250 ms window
    let t = DispatchSource.makeTimerSource(queue: tickQ); t.schedule(deadline: .now() + 0.25, repeating: 0.25)
    t.setEventHandler { [unowned self] in
      fLock.lock(); let n = frames; frames = 0; let prev = lastWindow; lastWindow = n; windows += 1; let w = windows; fLock.unlock()
      if n != prev || (n > 0 && w % 4 == 0) { log("frames \(n) /250ms") }
    }
    t.resume(); timer = t
  }
  var timer: DispatchSourceTimer?

  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) { fLock.lock(); frames += 1; fLock.unlock() }

  func start(_ why: String) {
    sessionQ.async { [self] in
      log("CALL startRunning (\(why)) | before: \(st())")
      let t = ProcessInfo.processInfo.systemUptime
      session.startRunning()
      log(String(format: "RET  startRunning (\(why)) %.0f ms | ", (ProcessInfo.processInfo.systemUptime - t) * 1000) + st())
    }
  }
  func stop(_ why: String) {
    sessionQ.async { [self] in
      log("CALL stopRunning (\(why)) | before: \(st())")
      let t = ProcessInfo.processInfo.systemUptime
      session.stopRunning()
      log(String(format: "RET  stopRunning (\(why)) %.0f ms | ", (ProcessInfo.processInfo.systemUptime - t) * 1000) + st())
    }
  }
  func after(_ s: Double, _ f: @escaping () -> Void) { DispatchQueue.main.asyncAfter(deadline: .now() + s, execute: f) }

  // ---- lifecycle ----
  var inBackground = false
  func didBecomeActive() {
    activeCount += 1
    log("SCENE didBecomeActive #\(activeCount) | \(st())")
    if activeCount == 1 { after(3) { log("BG-REQUEST | \(self.st())") }; return }
    if activeCount != 2 { return }
    switch mode {
    case "stopInBackground", "appNotif":
      after(3) { self.stop("fg+3s") }
      after(5) { self.start("fg+5s") }
      after(7) { self.stop("fg+7s (the later stop)") }
      after(9.5) { self.end() }
    case "stopWhileInterrupted":
      after(3) { self.start("fg+3s") }
      after(5) { self.stop("fg+5s") }
      after(7.5) { self.end() }
    default:  // keepRunning: no calls around the background; one stop at fg+4s to see whether it reaches the engine
      after(4) { self.stop("fg+4s (final)") }
      after(6.5) { self.end() }
    }
  }
  func willResignActive() { log("SCENE willResignActive | \(st())") }
  func didEnterBackground() {
    inBackground = true
    // keep the process running (and logging) while in the background; ended at foreground
    bgTask = UIApplication.shared.beginBackgroundTask(withName: "probe") { log("background task expired") }
    log("SCENE didEnterBackground | \(st())")
    if mode == "stopInBackground" { stop("sceneDidEnterBackground") }
    if mode == "stopWhileInterrupted" {
      // fallback when no WasInterrupted arrives (SIMCAM_OFF_INTERRUPTIONS, unarmed): stop 1.5 s after didEnterBackground
      after(1.5) { if !self.stopDoneInBg && self.inBackground { self.stopDoneInBg = true; log("no WasInterrupted within 1.5 s of didEnterBackground; stopping anyway"); self.stop("bg+1.5s, not interrupted") } }
    }
  }
  func interrupted() {
    guard mode == "stopWhileInterrupted", inBackground, !stopDoneInBg else { return }
    DispatchQueue.main.async {
      self.after(1) { if !self.stopDoneInBg { self.stopDoneInBg = true; self.stop("WasInterrupted+1s") } }
    }
  }
  func willEnterForeground() {
    inBackground = false
    log("SCENE willEnterForeground | \(st())")
    if mode == "stopInBackground" { start("sceneWillEnterForeground") }
    if bgTask != .invalid { UIApplication.shared.endBackgroundTask(bgTask); bgTask = .invalid }
  }
  func end() {
    sessionQ.async { log("END | \(self.st())") }
  }
}

let probe = Probe()
for (n, label) in [(UIApplication.didEnterBackgroundNotification, "didEnterBackground"), (UIApplication.willEnterForegroundNotification, "willEnterForeground"),
                   (UIApplication.didBecomeActiveNotification, "didBecomeActive"), (UIApplication.willResignActiveNotification, "willResignActive")] {
  NotificationCenter.default.addObserver(forName: n, object: nil, queue: nil) { _ in log("APP  \(label) notification") }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
    guard let ws = scene as? UIWindowScene else { return }
    let w = UIWindow(windowScene: ws); w.rootViewController = UIViewController(); w.rootViewController!.view.backgroundColor = .darkGray; w.makeKeyAndVisible()
    window = w
    log("SCENE willConnect")
    probe.setup()
  }
  func sceneDidBecomeActive(_ s: UIScene) { probe.didBecomeActive() }
  func sceneWillResignActive(_ s: UIScene) { probe.willResignActive() }
  func sceneDidEnterBackground(_ s: UIScene) { probe.didEnterBackground() }
  func sceneWillEnterForeground(_ s: UIScene) { probe.willEnterForeground() }
}
final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]?) -> Bool { log("APP  didFinishLaunching"); return true }
  func application(_ a: UIApplication, configurationForConnecting s: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: "Default", sessionRole: s.role); c.delegateClass = SceneDelegate.self; return c
  }
}
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
