// SPDX-License-Identifier: Apache-2.0
// main.swift: Stuntcam Camera, the camera helper (the ring contract it publishes is docs/webcam-ring.md).
// Publishes one Mac camera, or a synthetic pattern, into the shm ring that the engine's `webcam` source reads.
// Launched only through LaunchServices, so that it is its own responsible process for camera permission:
//   open -n -g "<absolute path>/Stuntcam Camera.app" --args --home "$SIMCAM_HOME" [options]
//   --home PATH        $SIMCAM_HOME (absolute). Part of the ring name; holds the lock, pid file, reader files and log.
//   --camera UNIQUEID  the camera, by AVCaptureDevice.uniqueID. Default: the built-in camera, else the first external
//                      one, never a Continuity Camera or Desk View. The default's uniqueID is written to webcam/default.
//   --synthetic        no camera and no permission request: a moving pattern with the latency stamp. Its uniqueID is
//                      "synthetic" unless --camera names one.
//   --size WxH         the largest capture format (default 1920x1080; also the synthetic frame size); --fps N (30)
//   --stamp            stamp camera frames too (latency tests)
//   --close-after S    close the camera S seconds after the last active reader (default 10)
//   --quit-after S     quit S seconds after the last reader of any kind (default 600)
//   --sha SHA          the <sha> recorded in the pid file (default: the name of the bundle's parent directory)
//   --jitter-ms N      tests only, synthetic: each frame is published 0-N ms (uniform) after its capture time, as a
//                      camera's publish times wander
//   --list PATH        write the cameras to PATH and exit: a first line "stuntcam-cameras 1", then one line per
//                      camera, tab-separated: uniqueID, default (1 for the camera chosen without --camera, else 0),
//                      Continuity Camera (1/0), device type, localized name. Takes no lock, writes no pid file or
//                      default file and asks for no permission (listing cameras needs none). Written through PATH.tmp
//                      and a rename.
//   --test-hooks       tests only: SIGUSR1 / SIGUSR2 act as the camera's disconnect / reconnect notifications (the
//                      synthetic source stops and starts as an unplugged camera would)
// Files under $SIMCAM_HOME/webcam/: <cam>.lock (held for life), <cam>.pid (spec §8), readers/ (spec §9), default.
// Log: $SIMCAM_HOME/logs/webcam-<cam>.log.
import AppKit
import AVFoundation
import CoreMedia
import Darwin

// ---- options
var home = "", cameraArg: String?, synthetic = false, maxW: UInt32 = 1920, maxH: UInt32 = 1080, fps = 30.0
var stampFrames = false, closeAfter = 10.0, quitAfter = 600.0, shaArg: String?, testHooks = false, jitterMs = 0.0,
    listPath: String?
do {
  var it = CommandLine.arguments.dropFirst().makeIterator()
  while let a = it.next() {
    switch a {
    case "--home": home = it.next() ?? ""
    case "--camera": cameraArg = it.next()
    case "--synthetic": synthetic = true
    case "--size":
      let p = (it.next() ?? "").split(separator: "x").compactMap { UInt32($0) }
      if p.count == 2, (1...8192).contains(p[0]), (1...8192).contains(p[1]) { maxW = p[0]; maxH = p[1] }
    case "--fps": fps = min(240, max(1, Double(it.next() ?? "") ?? 30))
    case "--stamp": stampFrames = true
    case "--close-after": closeAfter = max(0, Double(it.next() ?? "") ?? 10)
    case "--quit-after": quitAfter = max(1, Double(it.next() ?? "") ?? 600)
    case "--sha": shaArg = it.next()
    case "--test-hooks": testHooks = true
    case "--jitter-ms": jitterMs = min(30, max(0, Double(it.next() ?? "") ?? 0))
    case "--list": listPath = it.next() ?? ""
    default: break
    }
  }
}
guard home.hasPrefix("/") else {
  FileHandle.standardError.write("Stuntcam Camera: --home <absolute SIMCAM_HOME> is required\n".data(using: .utf8)!)
  exit(2)
}
while home.count > 1 && home.hasSuffix("/") { home.removeLast() }
let webcamDir = home + "/webcam", readersDir = webcamDir + "/readers", logsDir = home + "/logs"
// mkdir's result is deliberately unused: EEXIST is the usual case, and a directory that can't be made shows up at once
// as the lock failing (logged with its errno) or no log file
for d in [webcamDir, readersDir, logsDir] { _ = mkdir(d, 0o700) }
let startedUnix = UInt64(Date().timeIntervalSince1970)

// ---- the camera, chosen before anything is locked: its uniqueID names the lock, pid file and ring
func discovered() -> [AVCaptureDevice] {
  AVCaptureDevice.DiscoverySession(
    deviceTypes: [.builtInWideAngleCamera, .external, .continuityCamera, .deskViewCamera],
    mediaType: .video, position: .unspecified).devices
}
func isDefaultCandidate(_ d: AVCaptureDevice) -> Bool { !d.isContinuityCamera && d.deviceType != .deskViewCamera }
// the camera used without --camera: the built-in camera, else the first external one
func defaultCamera(_ all: [AVCaptureDevice]) -> AVCaptureDevice? {
  all.first { isDefaultCandidate($0) && $0.deviceType == .builtInWideAngleCamera }
    ?? all.first { isDefaultCandidate($0) && $0.deviceType == .external }
}
if let lp = listPath {
  guard lp.hasPrefix("/") else {
    FileHandle.standardError.write("Stuntcam Camera: --list needs an absolute path\n".data(using: .utf8)!)
    exit(2)
  }
  let all = discovered(), def = defaultCamera(all)
  func field(_ s: String) -> String {   // control characters as spaces: one camera per line, tab-separated
    String(s.unicodeScalars.map { $0.value < 0x20 || $0.value == 0x7f ? " " : Character($0) })
  }
  var out = "stuntcam-cameras 1\n"
  for d in all {
    out += "\(field(d.uniqueID))\t\(d.uniqueID == def?.uniqueID ? 1 : 0)\t\(d.isContinuityCamera ? 1 : 0)\t"
      + "\(field(d.deviceType.rawValue))\t\(field(d.localizedName))\n"
  }
  exit(sch_write_file(lp, out, strlen(out)) == 0 ? 0 : 1)
}
var device: AVCaptureDevice?
var uniqueID = "", deviceName = "Synthetic"
var earlyLog: [String] = []
if synthetic {
  uniqueID = cameraArg ?? "synthetic"
} else {
  let all = discovered()
  for d in all {
    earlyLog.append("camera '\(d.localizedName)' type=\(d.deviceType.rawValue) id=\(d.uniqueID)"
                    + " continuity=\(d.isContinuityCamera) default-candidate=\(isDefaultCandidate(d))")
  }
  if let id = cameraArg {
    uniqueID = id
    device = all.first { $0.uniqueID == id } ?? AVCaptureDevice(uniqueID: id)
  } else {
    device = defaultCamera(all)
    guard let d = device else {
      FileHandle.standardError.write("Stuntcam Camera: no camera found\n".data(using: .utf8)!)
      exit(1)
    }
    uniqueID = d.uniqueID
  }
  if let d = device { deviceName = d.localizedName }
}
let cam: String = {
  var b = [CChar](repeating: 0, count: 9)
  scw_cam_hash(home, uniqueID, &b)
  return String(cString: b)
}()
let shmName: String = {
  var b = [CChar](repeating: 0, count: 64)
  _ = scw_shm_name(&b, 64, UInt(getuid()), cam, 1)
  return String(cString: b)
}()
let pidPath = webcamDir + "/\(cam).pid", lockPath = webcamDir + "/\(cam).lock"
let sha: String = {
  let s = shaArg ?? URL(fileURLWithPath: Bundle.main.bundlePath).deletingLastPathComponent().lastPathComponent
  return s.count == 12 && s.allSatisfy({ "0123456789abcdef".contains($0) }) ? s : "000000000000"
}()

// ---- log
let logFD = open(logsDir + "/webcam-\(cam).log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o600)
let logLock = NSLock()
func log(_ s: String) {
  let line = String(format: "%.3f [helper %d] ", Date().timeIntervalSince1970, getpid()) + s + "\n"
  logLock.lock(); defer { logLock.unlock() }
  _ = line.withCString { write(logFD, $0, strlen($0)) }
}

// ---- lock: one helper per camera and $SIMCAM_HOME
let lockFD = sch_lock(lockPath)
if lockFD < 0 {
  log("another helper holds \(lockPath) (\(String(cString: strerror(errno)))); exiting")
  exit(0)
}
log("start \(Bundle.main.bundleIdentifier ?? "-") sha=\(sha) camera=\(uniqueID) '\(deviceName)' cam=\(cam) shm=\(shmName)"
    + " synthetic=\(synthetic) size<=\(maxW)x\(maxH)@\(fps) close-after=\(closeAfter)s quit-after=\(quitAfter)s"
    + " path=\(Bundle.main.bundlePath)")
for l in earlyLog { log(l) }
if cameraArg == nil && !synthetic {   // a synthetic helper never names the default camera
  let t = uniqueID + "\n"
  _ = sch_write_file(webcamDir + "/default", t, strlen(t))
}

// ---- shared state. Main queue: lifecycle, pid file. ringQ: everything that touches the ring and every frame.
let ringQ = DispatchQueue(label: "dev.stuntcam.camera.ring", qos: .userInteractive)
let sessionQ = DispatchQueue(label: "dev.stuntcam.camera.session", qos: .userInitiated)
var auth = Int32(SCW_AUTH_NOT_DETERMINED)    // main
var pidState = UInt32(SCW_STATE_STARTING)    // main: the state last written to the pid file
func writePid() {
  var p = scw_pidfile()
  sch_pidfile_fill(&p, getpid(), sha, auth, pidState, uniqueID, startedUnix)
  var b = [CChar](repeating: 0, count: 1024)
  let n = scw_pidfile_format(&b, 1024, &p)
  if n > 0 { _ = sch_write_file(pidPath, b, Int(n)) } else { log("pid file: format failed (\(n))") }
}

// ringQ-only
var writer: UnsafeMutablePointer<scw_shm_writer>?
var syncClock: CMClock?          // the capture session's clock, for converting sample times to the host clock
var ringW: UInt32 = 0, ringH: UInt32 = 0, seq: UInt64 = 0, ringState = UInt32(SCW_STATE_STARTING)
var capturing = false            // frames are accepted: set by Camera.start / synthStart, cleared by their stop
var lastFrameNs: UInt64 = 0, published = 0, statPub = 0, statCopyNs: UInt64 = 0, statDelayNs: Int64 = 0, statCam = 0

// seconds from b to a on the host clock (negative if a is earlier)
func secs(_ a: UInt64, _ b: UInt64) -> Double { Double(Int64(bitPattern: a &- b)) / 1e9 }
func setState(_ s: UInt32) {     // any queue
  ringQ.async {
    ringState = s
    if let w = writer { scw_writer_state(sch_ring(w), s) }
    DispatchQueue.main.async {
      if pidState != s {
        pidState = s
        writePid()
        log("state \(String(cString: scw_state_name(s)))")
      }
    }
  }
}
// ringQ: a new object at the name (new epoch, seq from 1), published before the old one is retired as superseded
func ringCreate(_ w: UInt32, _ h: UInt32) -> Bool {
  let bpr = w * 4
  let bytes = scw_object_bytes(h, bpr, 4, nil)
  guard bytes > 0 else { log("ring: geometry \(w)x\(h) out of range"); return false }
  let nw = UnsafeMutablePointer<scw_shm_writer>.allocate(capacity: 1)
  nw.initialize(to: scw_shm_writer())
  var rep = scw_shm_report()
  let rc = scw_shm_create(nw, shmName, bytes, &rep)
  if rc != SCW_OK {
    log("ring: \(String(cString: sch_report_msg(&rep))) (rc \(rc))")
    nw.deallocate()
    return false
  }
  let epoch = sch_epoch()
  let irc = scw_writer_init(sch_ring(nw), UnsafeMutableRawPointer(nw.pointee.h), nw.pointee.len, w, h, bpr, 4, epoch,
                            UInt32(fps * 1000), getpid(), deviceName)
  if irc != SCW_OK {
    log("ring: writer init \(irc)")
    scw_shm_retire(nw, UInt32(SCW_STATE_STOPPED))
    nw.deallocate()
    return false
  }
  scw_writer_state(sch_ring(nw), ringState)
  scw_writer_beat(sch_ring(nw), sch_now_ns())
  scw_writer_publish(sch_ring(nw))
  if let old = writer {
    scw_shm_retire(old, UInt32(SCW_STATE_SUPERSEDED))
    old.deallocate()
  }
  writer = nw
  ringW = w
  ringH = h
  seq = 0
  log(String(format: "ring %@ %ux%u BGRA, 4 slots, %llu bytes, epoch %016llx, mode %04o (fchmod errno %d)", shmName, w, h,
             bytes, epoch, rep.mode, rep.fchmod_errno))
  return true
}
func ringRetire(_ why: String) {   // ringQ
  capturing = false
  if let w = writer {
    scw_shm_retire(w, UInt32(SCW_STATE_STOPPED))
    w.deallocate()
    writer = nil
    log("ring retired (\(why)): slots cleared, name unlinked")
  }
}
// ringQ: one frame. `fill` writes the slot's pixels (bpr0 = 4 * width).
func publish(captureNs: UInt64, flags: UInt32, fill: (UnsafeMutablePointer<UInt8>) -> Void) {
  guard capturing, let w = writer else { return }
  let t0 = sch_now_ns()
  seq += 1
  guard let p = scw_writer_begin(sch_ring(w), seq) else { return }
  fill(p)
  if stampFrames || synthetic { sch_stamp(p, ringW * 4, ringW, ringH, seq, captureNs) }
  let pub = sch_now_ns()
  scw_writer_end(sch_ring(w), seq, captureNs, pub, flags)
  lastFrameNs = pub
  published += 1
  statPub += 1
  statCopyNs += pub - t0
  if ringState != UInt32(SCW_STATE_RUNNING) { setState(UInt32(SCW_STATE_RUNNING)) }
}

// ---- synthetic source (ringQ timer)
var synthTimer: DispatchSourceTimer?
var synth = sch_synth()
var synthFrame: UInt64 = 0
func synthStart() {   // ringQ
  if synthTimer != nil { return }
  if writer == nil || ringW != maxW || ringH != maxH { guard ringCreate(maxW, maxH) else { return } }
  capturing = true
  let t = DispatchSource.makeTimerSource(flags: .strict, queue: ringQ)
  t.schedule(deadline: .now(), repeating: .nanoseconds(Int(1e9 / fps)), leeway: .microseconds(200))
  t.setEventHandler {
    let cap = sch_now_ns()
    if jitterMs > 0 { usleep(useconds_t(Double.random(in: 0...jitterMs) * 1000)) }
    publish(captureNs: cap, flags: UInt32(SCW_SLOT_SYNTHETIC)) { p in
      sch_synth_draw(&synth, p, ringW * 4, ringW, ringH, synthFrame)
    }
    synthFrame += 1
  }
  t.resume()
  synthTimer = t
  log("synthetic source on (\(maxW)x\(maxH)@\(fps)\(jitterMs > 0 ? ", publish jitter 0-\(jitterMs) ms" : ""))")
}
func synthStop() {   // ringQ
  synthTimer?.cancel()
  synthTimer = nil
  capturing = false
}

// ---- camera source
func pickFormat(_ d: AVCaptureDevice) -> AVCaptureDevice.Format? {
  func dims(_ f: AVCaptureDevice.Format) -> CMVideoDimensions { CMVideoFormatDescriptionGetDimensions(f.formatDescription) }
  func rate(_ f: AVCaptureDevice.Format) -> Bool {
    f.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= fps - 0.5 && $0.minFrameRate <= fps + 0.5 }
  }
  let fit = d.formats.filter { dims($0).width <= Int32(maxW) && dims($0).height <= Int32(maxH) && rate($0) }
  if let best = fit.max(by: { Int(dims($0).width) * Int(dims($0).height) < Int(dims($1).width) * Int(dims($1).height) }) {
    return best
  }
  return d.formats.filter(rate).min(by: { Int(dims($0).width) * Int(dims($0).height) < Int(dims($1).width) * Int(dims($1).height) })
}
final class Camera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  var session: AVCaptureSession?
  var mismatchLogged = false, formatLogged = false
  // sessionQ
  func start(_ d: AVCaptureDevice) -> Bool {
    stop()
    ringQ.sync { capturing = true }   // in sessionQ order: a close queued before this start can't clear it afterwards
    let s = AVCaptureSession()
    do {
      AVCaptureDevice.centerStageControlMode = .app
      AVCaptureDevice.isCenterStageEnabled = false
      s.beginConfiguration()
      let input = try AVCaptureDeviceInput(device: d)
      guard s.canAddInput(input) else { log("camera: cannot add input"); return false }
      s.addInput(input)
      if let f = pickFormat(d) {
        try d.lockForConfiguration()
        d.activeFormat = f
        if f.videoSupportedFrameRateRanges.contains(where: { $0.maxFrameRate >= fps - 0.5 && $0.minFrameRate <= fps + 0.5 }) {
          d.activeVideoMinFrameDuration = CMTime(value: 1000, timescale: CMTimeScale(fps * 1000))
          d.activeVideoMaxFrameDuration = d.activeVideoMinFrameDuration
        }
        d.unlockForConfiguration()
      }
      let out = AVCaptureVideoDataOutput()
      out.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]   // the format's own size
      out.alwaysDiscardsLateVideoFrames = true
      out.setSampleBufferDelegate(self, queue: ringQ)
      guard s.canAddOutput(out) else { log("camera: cannot add output"); return false }
      s.addOutput(out)
      s.commitConfiguration()
      let dm = CMVideoFormatDescriptionGetDimensions(d.activeFormat.formatDescription)
      log("camera '\(d.localizedName)' format \(dm.width)x\(dm.height) minFD=\(CMTimeGetSeconds(d.activeVideoMinFrameDuration))"
          + " centerStage=\(AVCaptureDevice.isCenterStageEnabled)"
          + " reactionGestures=\(AVCaptureDevice.reactionEffectGesturesEnabled)"
          + " portrait=\(AVCaptureDevice.isPortraitEffectEnabled) studioLight=\(AVCaptureDevice.isStudioLightEnabled)")
      session = s
      NotificationCenter.default.addObserver(self, selector: #selector(runtimeError(_:)),
                                             name: AVCaptureSession.runtimeErrorNotification, object: s)
      // the clock is read before the first sample can arrive, so no frame is stamped with a stale or missing clock
      var clk = s.synchronizationClock
      let early = clk != nil
      ringQ.sync { syncClock = clk }
      s.startRunning()
      if clk == nil {
        clk = s.synchronizationClock
        ringQ.sync { syncClock = clk }
      }
      log("camera session running=\(s.isRunning) sync clock \(early ? "read before start" : clk != nil ? "read after start" : "missing")")
      return s.isRunning
    } catch { log("camera: setup failed: \(error)"); return false }
  }
  func stop() {   // sessionQ
    ringQ.sync { capturing = false }
    guard let s = session else { return }
    NotificationCenter.default.removeObserver(self, name: AVCaptureSession.runtimeErrorNotification, object: s)
    s.stopRunning()
    for o in s.outputs { s.removeOutput(o) }
    for i in s.inputs { s.removeInput(i) }
    session = nil
    log("camera session stopped")
  }
  @objc func runtimeError(_ n: Notification) {
    log("camera: runtime error \(String(describing: n.userInfo?[AVCaptureSessionErrorKey])); restarting in 1 s")
    DispatchQueue.main.asyncAfter(deadline: .now() + 1) { lifecycle.restartCapture("runtime error") }
  }
  // ringQ
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    guard capturing, let pb = CMSampleBufferGetImageBuffer(sb) else { return }
    let arrive = sch_now_ns()
    var pts = CMSampleBufferGetPresentationTimeStamp(sb)
    if let clk = syncClock { pts = CMSyncConvertTime(pts, from: clk, to: CMClockGetHostTimeClock()) }
    let capNs = UInt64(max(0, CMTimeConvertScale(pts, timescale: 1_000_000_000, method: .roundHalfAwayFromZero).value))
    guard CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA else {
      if !formatLogged {
        formatLogged = true
        log("camera: frame format is not BGRA; dropped")
      }
      return
    }
    let w = UInt32(CVPixelBufferGetWidth(pb)), h = UInt32(CVPixelBufferGetHeight(pb))
    if w != ringW || h != ringH {   // the camera delivers another size than the ring: a new object with that geometry
      if !mismatchLogged {
        mismatchLogged = true
        log("camera frames are \(w)x\(h), ring is \(ringW)x\(ringH): new ring")
      }
      guard ringCreate(w, h) else { return }
    }
    statCam += 1
    statDelayNs += Int64(arrive) - Int64(capNs)
    CVPixelBufferLockBaseAddress(pb, .readOnly); defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
    guard let base = CVPixelBufferGetBaseAddress(pb) else { return }
    let sbpr = CVPixelBufferGetBytesPerRow(pb)
    publish(captureNs: capNs, flags: 0) { p in
      sch_copy_rows(p, Int(w) * 4, base.assumingMemoryBound(to: UInt8.self), sbpr, Int(w) * 4, h)
    }
  }
}
let camera = Camera()

// ---- lifecycle (main queue)
final class Lifecycle: NSObject, NSApplicationDelegate {
  var wantOpen = false          // a reader is active: the camera (or synthetic source) should run
  var openSince: UInt64 = 0
  var disconnected = false
  var lastActiveNs = sch_now_ns(), lastReaderNs = sch_now_ns()
  var readersLogged = ""
  var timer: DispatchSourceTimer?, dirWatch: DispatchSourceFileSystemObject?, beat: DispatchSourceTimer?
  var sigs: [DispatchSourceSignal] = []
  var quitting = false

  func applicationDidFinishLaunching(_ n: Notification) {
    for s in [SIGTERM, SIGINT, SIGHUP] {
      signal(s, SIG_IGN)
      let src = DispatchSource.makeSignalSource(signal: s, queue: .main)
      src.setEventHandler { [unowned self] in self.quit(s == SIGTERM ? "SIGTERM" : s == SIGINT ? "SIGINT" : "SIGHUP", code: 0) }
      src.resume()
      sigs.append(src)
    }
    if testHooks {
      for s in [SIGUSR1, SIGUSR2] {
        signal(s, SIG_IGN)
        let src = DispatchSource.makeSignalSource(signal: s, queue: .main)
        src.setEventHandler { [unowned self] in self.simulatePlug(s == SIGUSR2) }
        src.resume()
        sigs.append(src)
      }
    }
    // the ring exists from the start, so readers see why it is offline (starting, prompting, idle, denied)
    var ok = false, w0 = maxW, h0 = maxH
    if let d = device, let f = pickFormat(d) {
      let dm = CMVideoFormatDescriptionGetDimensions(f.formatDescription)
      w0 = UInt32(dm.width)
      h0 = UInt32(dm.height)
    }
    ringQ.sync { ok = ringCreate(w0, h0) }
    if !ok {
      writePid()
      log("no ring; exiting")
      quit("ring create failed", code: 1)
      return
    }
    writePid()
    let b = DispatchSource.makeTimerSource(queue: ringQ)
    b.schedule(deadline: .now(), repeating: .milliseconds(100))
    b.setEventHandler { if let w = writer { scw_writer_beat(sch_ring(w), sch_now_ns()) } }
    b.resume()
    beat = b
    let nc = NotificationCenter.default
    nc.addObserver(self, selector: #selector(deviceDisconnected(_:)), name: AVCaptureDevice.wasDisconnectedNotification,
                   object: nil)
    nc.addObserver(self, selector: #selector(deviceConnected(_:)), name: AVCaptureDevice.wasConnectedNotification, object: nil)
    NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(didWake(_:)),
                                                      name: NSWorkspace.didWakeNotification, object: nil)
    settlePermission()
    let t = DispatchSource.makeTimerSource(queue: .main)
    t.schedule(deadline: .now() + 0.2, repeating: 1.0)
    t.setEventHandler { [unowned self] in self.tick() }
    t.resume()
    timer = t
    let dfd = open(readersDir, O_EVTONLY)
    if dfd >= 0 {   // a new or rewritten reader file wakes the scan at once, not at the next 1 s tick
      let w = DispatchSource.makeFileSystemObjectSource(fileDescriptor: dfd, eventMask: .write, queue: .main)
      w.setEventHandler { [unowned self] in self.tick() }
      w.setCancelHandler { close(dfd) }
      w.resume()
      dirWatch = w
    }
  }

  // Permission is settled at launch, before any reader exists. Synthetic mode only reports it.
  func settlePermission() {
    let st = AVCaptureDevice.authorizationStatus(for: .video)
    func authCode(_ s: AVAuthorizationStatus) -> Int32 {
      switch s {
      case .authorized: return Int32(SCW_AUTH_AUTHORIZED)
      case .notDetermined: return Int32(SCW_AUTH_NOT_DETERMINED)
      default: return Int32(SCW_AUTH_DENIED)
      }
    }
    auth = authCode(st)
    writePid()
    log("camera permission: \(st == .authorized ? "authorized" : st == .notDetermined ? "notDetermined" : st == .denied ? "denied" : "restricted")")
    if synthetic {
      setState(UInt32(SCW_STATE_IDLE))
      return
    }
    switch st {
    case .authorized: setState(UInt32(SCW_STATE_IDLE))
    case .notDetermined:
      auth = Int32(SCW_AUTH_PROMPTING)
      setState(UInt32(SCW_STATE_PROMPTING))
      writePid()
      log("requesting camera access (a dialog is shown)")
      AVCaptureDevice.requestAccess(for: .video) { granted in
        DispatchQueue.main.async {
          log("camera access \(granted ? "granted" : "denied")")
          if granted {
            auth = Int32(SCW_AUTH_AUTHORIZED)
            writePid()
            setState(UInt32(SCW_STATE_IDLE))
            self.tick()
          } else {
            self.denied()
          }
        }
      }
    default: denied()
    }
  }
  func denied() {
    auth = Int32(SCW_AUTH_DENIED)
    setState(UInt32(SCW_STATE_DENIED))
    ringQ.sync {}
    DispatchQueue.main.async { writePid() }
    log("camera access denied or restricted: System Settings > Privacy & Security > Camera > Stuntcam Camera; exiting")
    // the pid file stays (state=denied) for the CLI to read; the ring holds no frame and says denied
    DispatchQueue.main.asyncAfter(deadline: .now() + 3) { exit(0) }
  }
  var canCapture: Bool { synthetic || auth == Int32(SCW_AUTH_AUTHORIZED) }

  struct Readers { var any = false, active = false, list: [String] = [] }
  func scanReaders() -> Readers {
    var r = Readers()
    let names = (try? FileManager.default.contentsOfDirectory(atPath: readersDir)) ?? []
    for n in names where !n.hasSuffix(".tmp") {
      guard let dash = n.lastIndex(of: "-"), let pid = Int32(n[n.index(after: dash)...]) else { continue }
      let path = readersDir + "/" + n
      let age = sch_age_s(path)
      guard sch_pid_alive(pid) != 0 else {
        if age > 5 { unlink(path) }   // its process is gone
        continue
      }
      // live engines rewrite theirs every second: an older file (an app stopped in a debugger, a reused pid) doesn't count
      if age > 3 || age < -3 {
        r.list.append("\(n)(stale)")
        continue
      }
      var b = [CChar](repeating: 0, count: 512)
      guard sch_read_small(path, &b, 512) >= 0 else { continue }
      var rings: [UInt32] = [], active = false, want = "-"
      for line in String(cString: b).split(separator: "\n") {
        let kv = line.split(separator: "=", maxSplits: 1).map(String.init)
        guard kv.count == 2 else { continue }
        switch kv[0] {
        case "rings": rings = kv[1].split(separator: ",").compactMap { UInt32($0) }
        case "active": active = kv[1] == "1"
        case "want": want = kv[1]
        default: break
        }
      }
      r.any = true
      if active && rings.contains(1) { r.active = true }
      r.list.append("\(n)(active=\(active ? 1 : 0) want=\(want)"
                    + "\(rings.contains(1) ? "" : " rings=\(rings) unsupported"))")
    }
    return r
  }

  func tick() {
    if quitting { return }
    let now = sch_now_ns(), r = scanReaders()
    let desc = r.list.sorted().joined(separator: " ")
    if desc != readersLogged {
      readersLogged = desc
      log("readers: \(desc.isEmpty ? "none" : desc)")
    }
    if r.any { lastReaderNs = now }
    if r.active { lastActiveNs = now }
    if r.active && !wantOpen && canCapture {
      openCapture()
    } else if !r.active && wantOpen && secs(now, lastActiveNs) >= closeAfter {
      closeCapture("no active reader for \(closeAfter) s")
    }
    if !r.any && secs(now, lastReaderNs) >= quitAfter {
      quit("no reader for \(quitAfter) s", code: 0)
      return
    }
    // liveness: frames must keep coming while a reader is active
    if wantOpen && !disconnected && secs(sch_now_ns(), openSince) > 2 {
      var last: UInt64 = 0
      ringQ.sync { last = lastFrameNs }
      if last < openSince || secs(sch_now_ns(), last) > 2 { restartCapture("no frame for 2 s") }
    }
    ringQ.async {
      if statPub > 0 || statCam > 0 {
        log(String(format: "stat published %d (seq %llu) copy %.2f ms camera->helper %.1f ms", statPub, seq,
                   Double(statCopyNs) / Double(max(statPub, 1)) / 1e6, Double(statDelayNs) / Double(max(statCam, 1)) / 1e6))
      }
      statPub = 0
      statCam = 0
      statCopyNs = 0
      statDelayNs = 0
    }
  }

  func openCapture() {
    wantOpen = true
    openSince = sch_now_ns()
    log("opening the \(synthetic ? "synthetic source" : "camera") (active reader)")
    // camera: in sessionQ order, after the idle state of a close still queued there
    if synthetic {
      setState(UInt32(SCW_STATE_STARTING))
    } else {
      sessionQ.async { setState(UInt32(SCW_STATE_STARTING)) }
    }
    if synthetic && !simUnplugged {
      ringQ.async { synthStart() }
      return
    }
    // a synthetic source or a camera unplugged by --test-hooks, or a camera that is absent or not connected: wait for it
    guard !synthetic, let d = device ?? AVCaptureDevice(uniqueID: uniqueID), d.isConnected, !simUnplugged else {
      disconnected = true
      log("camera \(uniqueID) is not connected; waiting for it")
      return
    }
    device = d
    disconnected = false
    sessionQ.async { if !camera.start(d) { log("camera start failed; retrying at the liveness check") } }
  }
  // stops capture, then clears the slots: no frame outlives the camera session in shared memory
  func closeCapture(_ why: String) {
    wantOpen = false
    log("closing the \(synthetic ? "synthetic source" : "camera") (\(why))")
    let done = {
      ringQ.async {
        if let w = writer { scw_writer_clear(sch_ring(w)) }
        setState(UInt32(SCW_STATE_IDLE))
      }
    }
    if synthetic {
      ringQ.async { synthStop() }
      done()
      return
    }
    sessionQ.async {
      camera.stop()
      done()
    }
  }
  func restartCapture(_ why: String) {
    guard wantOpen, !quitting else { return }
    log("restarting capture (\(why))")
    openSince = sch_now_ns()
    if synthetic {
      if !simUnplugged {
        ringQ.async {
          synthStop()
          synthStart()
        }
      }
      return
    }
    guard let d = AVCaptureDevice(uniqueID: uniqueID), d.isConnected, !simUnplugged else {
      disconnected = true
      log("camera not connected; waiting for it")
      return
    }
    device = d
    sessionQ.async { if !camera.start(d) { log("camera restart failed") } }
  }
  @objc func deviceDisconnected(_ n: Notification) {
    guard let d = n.object as? AVCaptureDevice, d.uniqueID == uniqueID else { return }
    cameraGone()
  }
  @objc func deviceConnected(_ n: Notification) {
    guard let d = n.object as? AVCaptureDevice, d.uniqueID == uniqueID else { return }
    cameraBack(d)
  }
  // the camera went away: capture stops and, as at any close, its frames leave shared memory
  func cameraGone() {
    log("camera disconnected")
    disconnected = true
    let clear = {
      ringQ.async {
        if let w = writer { scw_writer_clear(sch_ring(w)) }
        setState(UInt32(SCW_STATE_STARTING))
      }
    }
    if synthetic {
      ringQ.async { synthStop() }
      clear()
    } else {
      sessionQ.async {
        camera.stop()
        clear()
      }
    }
  }
  func cameraBack(_ d: AVCaptureDevice?) {
    log("camera connected again")
    if let d = d { device = d }
    disconnected = false
    guard wantOpen else { return }
    openSince = sch_now_ns()
    if synthetic {
      ringQ.async { synthStart() }
    } else if let d = device {
      sessionQ.async { _ = camera.start(d) }
    }
  }
  // --test-hooks: SIGUSR1 = unplug, SIGUSR2 = plug back in (the device itself stays connected; simUnplugged hides it)
  var simUnplugged = false
  func simulatePlug(_ plugged: Bool) {
    log("test hook: simulated \(plugged ? "reconnect" : "disconnect")")
    simUnplugged = !plugged
    if plugged { cameraBack(nil) } else { cameraGone() }
  }
  @objc func didWake(_ n: Notification) {
    log("Mac woke")
    restartCapture("wake")
  }

  // retire: stop capture, mark the ring stopped, clear the slots, unlink the name, remove the pid file
  func quit(_ why: String, code: Int32) {
    if quitting { return }
    quitting = true
    log("quitting (\(why))")
    timer?.cancel()
    dirWatch?.cancel()
    if !synthetic { sessionQ.sync { camera.stop() } }
    ringQ.sync {
      synthStop()
      ringRetire(why)
    }
    pidState = UInt32(SCW_STATE_STOPPED)
    unlink(pidPath)   // still under the lock, so no new helper's file is removed
    log("exit \(code)")
    exit(code)
  }
  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    quit("terminate", code: 0)
    return .terminateNow
  }
}
let lifecycle = Lifecycle()
let app = NSApplication.shared
app.delegate = lifecycle
app.setActivationPolicy(.accessory)
app.run()
