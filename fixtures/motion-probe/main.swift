// SPDX-License-Identifier: Apache-2.0
// motion-probe (design U2): two CMMotionManager instances recording accelerometer updates at an IMU rate, optionally
// next to a running AVCaptureSession, in either start order. A measurement app: it logs, it checks nothing. README.md.
// Launch arguments: -camera 0|1 (default 0)  -order motionFirst|cameraFirst (default motionFirst)  -dur <s> (10)  -hz <Hz> (100)
import UIKit
import AVFoundation
import CoreMotion

let T0 = ProcessInfo.processInfo.systemUptime
func up() -> TimeInterval { ProcessInfo.processInfo.systemUptime }
func P(_ s: String) { print(String(format: "PROBE| %8.3f ", up() - T0) + s); fflush(stdout) }
let defs = UserDefaults.standard
let useCamera = defs.string(forKey: "camera") == "1"
let order = defs.string(forKey: "order") ?? "motionFirst"
let dur = defs.double(forKey: "dur") > 0 ? defs.double(forKey: "dur") : 10
let hz = defs.double(forKey: "hz") > 0 ? defs.double(forKey: "hz") : 100
func f(_ x: Double, _ d: Int = 3) -> String { String(format: "%.\(d)f", x) }
func avail(_ m: CMMotionManager) -> String {
  "accel=\(m.isAccelerometerAvailable) gyro=\(m.isGyroAvailable) mag=\(m.isMagnetometerAvailable) dm=\(m.isDeviceMotionAvailable)"
}

// One manager, push updates on its own serial OperationQueue. Samples are only touched on that queue.
final class Recorder {
  let name: String; let mgr = CMMotionManager(); let q = OperationQueue()
  var ts: [Double] = [], arr: [Double] = []; var startAt = 0.0, endAt = 0.0; var first: CMAcceleration?; var errors = 0
  init(_ name: String) { self.name = name; q.name = "motion.\(name)"; q.maxConcurrentOperationCount = 1; ts.reserveCapacity(4000); arr.reserveCapacity(4000) }
  func start() {
    mgr.accelerometerUpdateInterval = 1 / hz
    startAt = up(); endAt = startAt + dur
    mgr.startAccelerometerUpdates(to: q) { [self] d, e in
      if e != nil { errors += 1 }
      guard let d else { return }
      arr.append(up()); ts.append(d.timestamp); if first == nil { first = d.acceleration }
    }
    P("\(name) start interval=\(f(mgr.accelerometerUpdateInterval, 4)) accelActive=\(mgr.isAccelerometerActive)")
  }
  func report() {
    let pull = mgr.accelerometerData
    mgr.stopAccelerometerUpdates(); q.waitUntilAllOperationsAreFinished()
    let n = arr.count; var inWin = 0; var maxGap = 0.0, minIv = Double.infinity, ivSum = 0.0, ivN = 0, nonMono = 0, maxOff = 0.0
    var prevArr = startAt
    for i in 0..<n where arr[i] <= endAt {
      inWin += 1; maxGap = max(maxGap, arr[i] - prevArr); prevArr = arr[i]
      maxOff = max(maxOff, abs(arr[i] - ts[i]))
      if i > 0 { let iv = ts[i] - ts[i - 1]; if iv <= 0 { nonMono += 1 }; minIv = min(minIv, iv); ivSum += iv; ivN += 1 }
    }
    maxGap = max(maxGap, endAt - prevArr)   // a stream that stops early shows as a gap to the end of the window
    let rate = Double(inWin) / dur
    let off0 = n > 0 ? arr[0] - ts[0] : .nan, offL = inWin > 0 ? arr[inWin - 1] - ts[inWin - 1] : .nan
    let g = first.map { "\(f($0.x, 2)),\(f($0.y, 2)),\(f($0.z, 2))" } ?? "nil"
    P("MGR \(name) count=\(inWin) total=\(n) rate=\(f(rate, 1)) pct=\(f(100 * rate / hz, 1)) meanIv=\(ivN > 0 ? f(1000 * ivSum / Double(ivN), 2) : "nan")ms"
      + " minIv=\(ivN > 0 ? f(1000 * minIv, 2) : "nan")ms maxGap=\(f(1000 * maxGap, 1))ms firstLatency=\(n > 0 ? f(1000 * (arr[0] - startAt), 1) : "nan")ms"
      + " tsOffFirst=\(f(1000 * off0, 3))ms tsOffLast=\(f(1000 * offL, 3))ms tsOffMax=\(f(1000 * maxOff, 3))ms tsFirst=\(n > 0 ? f(ts[0]) : "nan")"
      + " upAtFirst=\(n > 0 ? f(arr[0]) : "nan") nonMonotonic=\(nonMono) errors=\(errors) gravity=\(g) pull=\(pull != nil)")
  }
}

final class Camera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  let session = AVCaptureSession(); let sq = DispatchQueue(label: "cam.session"); let vq = DispatchQueue(label: "cam.video")
  var frames: [Double] = []
  func start(_ then: @escaping () -> Void) {
    sq.async { [self] in
      let t = up()
      guard let dev = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else { P("CAM no back camera"); then(); return }
      session.beginConfiguration()
      if session.canSetSessionPreset(.hd1920x1080) { session.sessionPreset = .hd1920x1080 }
      if let inp = try? AVCaptureDeviceInput(device: dev), session.canAddInput(inp) { session.addInput(inp) }
      let out = AVCaptureVideoDataOutput(); out.setSampleBufferDelegate(self, queue: vq)
      if session.canAddOutput(out) { session.addOutput(out) }
      if (try? dev.lockForConfiguration()) != nil {
        if dev.activeFormat.videoSupportedFrameRateRanges.contains(where: { $0.minFrameRate <= 30 && 30 <= $0.maxFrameRate }) {
          dev.activeVideoMinFrameDuration = CMTime(value: 1, timescale: 30); dev.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: 30) }
        dev.unlockForConfiguration() }
      session.commitConfiguration()
      session.startRunning()
      let d = CMVideoFormatDescriptionGetDimensions(dev.activeFormat.formatDescription)
      P("CAM running=\(session.isRunning) device=\(dev.uniqueID) preset=\(session.sessionPreset.rawValue) format=\(d.width)x\(d.height) startTook=\(f(1000 * (up() - t), 1))ms")
      then()
    }
  }
  func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
    if frames.isEmpty { P("CAM first frame") }
    frames.append(up())
  }
  func report(from a: Double, to b: Double) {
    sq.sync { session.stopRunning() }
    let fr = vq.sync { frames }
    let inWin = fr.filter { $0 >= a && $0 <= b }.count
    P("CAM frames=\(fr.count) inWindow=\(inWin) fps=\(f(Double(inWin) / (b - a), 1)) firstFrameAt=\(fr.first.map { f($0 - T0) } ?? "nan")")
  }
}

final class VC: UIViewController {
  let a = Recorder("A"), b = Recorder("B"); var cam: Camera?
  var started = false
  override func viewDidAppear(_ animated: Bool) {
    super.viewDidAppear(animated)
    guard !started else { return }; started = true
    if let m = defs.string(forKey: "attitude"), m == "1" || m == "stock" {
      DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { AttitudeProbe(stock: m == "stock").run() }; return
    }
    P("pid=\(getpid()) camera=\(useCamera ? 1 : 0) order=\(order) dur=\(f(dur, 1)) hz=\(f(hz, 0)) os=\(UIDevice.current.systemVersion)")
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.begin() }
  }
  func startMotion() {
    P("AVAIL@first \(avail(a.mgr))")   // the app's first CoreMotion query (before any start)
    a.start(); b.start()
    P("ACTIVE@start A.accel=\(a.mgr.isAccelerometerActive) B.accel=\(b.mgr.isAccelerometerActive)")
    DispatchQueue.main.asyncAfter(deadline: .now() + dur + 0.05) { self.finish() }
  }
  func begin() {
    // the Camera (and so its AVCaptureSession, a trapped entry point) is created only at the camera step
    if useCamera && order == "cameraFirst" {
      P("STEP camera"); let c = Camera(); cam = c
      c.start { DispatchQueue.main.async { P("STEP motion") }; DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.startMotion() } }
    } else {
      P("STEP motion"); startMotion()
      if useCamera { DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { P("STEP camera"); let c = Camera(); self.cam = c; c.start {} } }
    }
  }
  func finish() {
    a.report(); b.report()
    cam?.report(from: a.startAt, to: a.endAt)
    // availability on a fresh manager after the window (after arming, if the camera ran), then start attempts for the others
    let c = CMMotionManager()
    P("AVAIL@end \(avail(c))")
    var g = 0, m = 0, dm = 0; let q = OperationQueue(); q.maxConcurrentOperationCount = 1
    c.gyroUpdateInterval = 1 / hz; c.magnetometerUpdateInterval = 1 / hz; c.deviceMotionUpdateInterval = 1 / hz
    c.startGyroUpdates(to: q) { d, _ in if d != nil { g += 1 } }
    c.startMagnetometerUpdates(to: q) { d, _ in if d != nil { m += 1 } }
    c.startDeviceMotionUpdates(to: q) { d, _ in if d != nil { dm += 1 } }
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
      P("ACTIVE@attempt gyro=\(c.isGyroActive) mag=\(c.isMagnetometerActive) dm=\(c.isDeviceMotionActive) accel=\(c.isAccelerometerActive)")
      DispatchQueue.main.asyncAfter(deadline: .now() + 1.7) {
        c.stopGyroUpdates(); c.stopMagnetometerUpdates(); c.stopDeviceMotionUpdates(); q.waitUntilAllOperationsAreFinished()
        P("DELIVERED@attempt 2s gyro=\(g) mag=\(m) dm=\(dm) gyroData=\(c.gyroData != nil) magData=\(c.magnetometerData != nil) dmData=\(c.deviceMotion != nil)")
        P("END"); exit(0)
      }
    }
  }
}

// ---- -attitude 1|stock: the device-motion attitude stand-in ----
// -attitude 1: CMMotionManager.availableAttitudeReferenceFrames, then startDeviceMotionUpdates(using: .xArbitraryZVertical,
// to:) at -hz for 2 s (-dur is ignored), a queue-less startDeviceMotionUpdates(using:) read after 0.5 s, and
// attitudeReferenceFrame. Every sample is checked:
//   non-nil attitude; |q| = 1; rotationMatrix == the matrix of q and of roll/pitch/yaw (Apple's conventions: R is the
//   transpose of q's matrix, q = qz(yaw) qx(pitch) qy(roll)); R * (0, 0, -1) == gravity (1e-3); gravity == the axis of
//   -expect portrait|portraitUpsideDown|landscapeLeft|landscapeRight (1e-3); every sample equal to the first (1e-6: held
//   still); magneticField zero and uncalibrated; and against a REAL CMAttitude built from the same quaternion with
//   CoreMotion's own (private) -initWithQuaternion: (a test oracle only): its rotationMatrix, its angles' matrix,
//   multiplyByInverseOfAttitude:. Then copy, a * a^-1, and coding: the archive is byte-identical to Apple's for the
//   same q and, with the stand-in active, decodes to an M5Attitude (M1: m1.multiply, m1.isEqual, m1.decodeClass).
//   24 checks in all. -m1 a|b|c: one M1 step only, then END (a throwaway measurement launch).
// The rate must be >= 90% of -hz. Lines: "ATT PASS|FAIL <check> ...", then "ATT SUMMARY pass=<n> fail=<n>".
// -armcam 1: one camera call before the motion calls (the control file's orientation= applies only once the camera arms).
// -attitude stock: CoreMotion is expected stock (a marker): no samples, a nil pull, the stock frames value is printed, and
//   an archive of Apple's CMAttitude decodes as CMAttitude (stock.decode: the engine maps the name only with the stand-in).
typealias Mat = [[Double]]
func matOf(_ r: CMRotationMatrix) -> Mat { [[r.m11, r.m12, r.m13], [r.m21, r.m22, r.m23], [r.m31, r.m32, r.m33]] }
func qMat(_ q: CMQuaternion) -> Mat {   // reference -> device: the transpose of q's rotation matrix
  let (x, y, z, w) = (q.x, q.y, q.z, q.w)
  return [[1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)],
          [2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)],
          [2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]]
}
func mmul(_ a: Mat, _ b: Mat) -> Mat { (0..<3).map { i in (0..<3).map { j in (0..<3).reduce(0) { $0 + a[i][$1] * b[$1][j] } } } }
func tr(_ a: Mat) -> Mat { (0..<3).map { i in (0..<3).map { j in a[j][i] } } }
func eulerMat(roll r: Double, pitch p: Double, yaw y: Double) -> Mat {   // R = (Rz(yaw) Rx(pitch) Ry(roll))^T
  let rz: Mat = [[cos(y), -sin(y), 0], [sin(y), cos(y), 0], [0, 0, 1]]
  let rx: Mat = [[1, 0, 0], [0, cos(p), -sin(p)], [0, sin(p), cos(p)]]
  let ry: Mat = [[cos(r), 0, sin(r)], [0, 1, 0], [-sin(r), 0, cos(r)]]
  return tr(mmul(rz, mmul(rx, ry)))
}
func mdiff(_ a: Mat, _ b: Mat) -> Double { (0..<3).flatMap { i in (0..<3).map { j in abs(a[i][j] - b[i][j]) } }.max()! }
func qdiff(_ a: CMQuaternion, _ b: CMQuaternion) -> Double { [abs(a.x - b.x), abs(a.y - b.y), abs(a.z - b.z), abs(a.w - b.w)].max()! }
func qs(_ q: CMQuaternion) -> String { "(\(f(q.x, 4)),\(f(q.y, 4)),\(f(q.z, 4)),\(f(q.w, 4)))" }
// a real CMAttitude: CoreMotion's private -initWithQuaternion: takes the quaternion as (w, x, y, z)
func appleAttitude(_ q: CMQuaternion) -> CMAttitude? {
  let sel = NSSelectorFromString("initWithQuaternion:")
  guard let cls = NSClassFromString("CMAttitude"), let m = class_getInstanceMethod(cls, sel),
        let alloc = (cls as AnyObject).perform(NSSelectorFromString("alloc")) else { return nil }
  typealias F = @convention(c) (UnsafeMutableRawPointer, Selector, CMQuaternion) -> UnsafeMutableRawPointer?
  let fn = unsafeBitCast(method_getImplementation(m), to: F.self)
  guard let r = fn(alloc.toOpaque(), sel, CMQuaternion(x: q.w, y: q.x, z: q.y, w: q.z)) else { return nil }
  return Unmanaged<AnyObject>.fromOpaque(r).takeRetainedValue() as? CMAttitude
}
final class AttitudeProbe {
  let stock: Bool; var pass = 0, fail = 0
  let mgr = CMMotionManager(), pull = CMMotionManager(), q = OperationQueue()
  var samples: [CMDeviceMotion] = []; var arr: [Double] = []
  init(stock: Bool) { self.stock = stock; q.maxConcurrentOperationCount = 1 }
  func R(_ ok: Bool, _ name: String, _ detail: String) { if ok { pass += 1 } else { fail += 1 }; P("ATT \(ok ? "PASS" : "FAIL") \(name) \(detail)") }
  func run() {
    let expect = defs.string(forKey: "expect") ?? "portrait"
    let frames = CMMotionManager.availableAttitudeReferenceFrames()
    P("ATT pid=\(getpid()) os=\(UIDevice.current.systemVersion) stock=\(stock) expect=\(expect) hz=\(f(hz, 0)) frames=\(frames.rawValue)")
    // -armcam 1: one camera call first, so the engine arms and reads its control file (orientation=, what `stuntcam orient`
    // writes): the control file is read only at activation, so an app that never touches the camera doesn't follow it
    if defs.string(forKey: "armcam") == "1" { let dev = AVCaptureDevice.default(for: .video); P("ATT armcam device=\(dev?.uniqueID ?? "nil")") }
    if !stock {
      R(frames.contains(.xArbitraryZVertical) && !frames.contains(.xMagneticNorthZVertical) && !frames.contains(.xTrueNorthZVertical), "frames", "available=\(frames.rawValue) (xArbitraryZVertical, no magnetic frames)")
      R(mgr.isDeviceMotionAvailable, "available", "dm=\(mgr.isDeviceMotionAvailable)")
    }
    mgr.deviceMotionUpdateInterval = 1 / hz
    let t0 = up()
    mgr.startDeviceMotionUpdates(using: .xArbitraryZVertical, to: q) { [self] d, _ in if let d { samples.append(d); arr.append(up()) } }
    pull.deviceMotionUpdateInterval = 1 / hz
    pull.startDeviceMotionUpdates(using: .xArbitraryZVertical)
    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [self] in
      let pd = pull.deviceMotion
      let pa = pd.flatMap { ($0 as NSObject).value(forKey: "attitude") as? CMAttitude }
      if stock { R(pd == nil, "stock.pull", "deviceMotion=\(pd == nil ? "nil" : "non-nil")") }
      else { R(pa != nil, "pull", "queue-less reference-frame start: deviceMotion=\(pd != nil) attitude=\(pa.map { qs($0.quaternion) } ?? "nil") frame=\(pull.attitudeReferenceFrame.rawValue)") }
      pull.stopDeviceMotionUpdates()
    }
    DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [self] in
      mgr.stopDeviceMotionUpdates(); q.waitUntilAllOperationsAreFinished()
      let n = arr.filter { $0 <= t0 + 2.0 }.count
      if stock {
        R(samples.isEmpty, "stock.updates", "samples=\(samples.count) frame=\(mgr.attitudeReferenceFrame.rawValue)")
        // M1: with CoreMotion stock the engine leaves NSKeyedUnarchiver's class map alone: Apple's archive decodes as Apple's
        let o = appleAttitude(CMQuaternion(x: 0, y: 0, z: 0, w: 1)), d = o.flatMap { try? NSKeyedArchiver.archivedData(withRootObject: $0, requiringSecureCoding: true) }
        let back = d.flatMap { try? NSKeyedUnarchiver.unarchivedObject(ofClass: CMAttitude.self, from: $0) }
        R(back.map { NSStringFromClass(type(of: $0)) == "CMAttitude" } ?? false, "stock.decode", "decoded class=\(back.map { NSStringFromClass(type(of: $0)) } ?? "nil") mapped=\(NSKeyedUnarchiver.class(forClassName: "CMAttitude").map { NSStringFromClass($0) } ?? "none")")
        finish(); return
      }
      R(Double(n) >= 0.9 * hz * 2, "rate", "samples=\(n) in 2 s, want>=\(Int(0.9 * hz * 2)) (\(f(Double(n) / 2 / hz * 100, 1))% of \(f(hz, 0)) Hz) frame=\(mgr.attitudeReferenceFrame.rawValue)")
      check(expect); finish()
    }
  }
  func check(_ expect: String) {
    guard let d0 = samples.first else { R(false, "samples", "none"); return }
    let want: [Double] = expect == "landscapeLeft" ? [-1, 0, 0] : expect == "landscapeRight" ? [1, 0, 0] : expect == "portraitUpsideDown" ? [0, 1, 0] : [0, -1, 0]
    var nilAtt = 0, badNorm = 0.0, badQ = 0.0, badE = 0.0, badG = 0.0, badW = 0.0, drift = 0.0, badMag = 0, badOracleR = 0.0, badOracleE = 0.0, badOracleAng = 0.0
    let a0 = (d0 as NSObject).value(forKey: "attitude") as? CMAttitude
    for d in samples {
      guard let a = (d as NSObject).value(forKey: "attitude") as? CMAttitude else { nilAtt += 1; continue }
      let qq = a.quaternion, rm = matOf(a.rotationMatrix), g = d.gravity
      badNorm = max(badNorm, abs(sqrt(qq.x * qq.x + qq.y * qq.y + qq.z * qq.z + qq.w * qq.w) - 1))
      badQ = max(badQ, mdiff(rm, qMat(qq)))
      badE = max(badE, mdiff(rm, eulerMat(roll: a.roll, pitch: a.pitch, yaw: a.yaw)))
      let down = [-rm[0][2], -rm[1][2], -rm[2][2]]   // R * (0, 0, -1)
      badG = max(badG, abs(down[0] - g.x), abs(down[1] - g.y), abs(down[2] - g.z))
      badW = max(badW, abs(g.x - want[0]), abs(g.y - want[1]), abs(g.z - want[2]))
      if let a0 { drift = max(drift, qdiff(qq, a0.quaternion), abs(a.roll - a0.roll), abs(a.pitch - a0.pitch), abs(a.yaw - a0.yaw), abs(g.x - d0.gravity.x), abs(g.y - d0.gravity.y), abs(g.z - d0.gravity.z)) }
      let mf = d.magneticField; if mf.accuracy != .uncalibrated || mf.field.x != 0 || mf.field.y != 0 || mf.field.z != 0 { badMag += 1 }
      if let o = appleAttitude(qq) {
        badOracleR = max(badOracleR, mdiff(rm, matOf(o.rotationMatrix)))
        badOracleE = max(badOracleE, mdiff(eulerMat(roll: o.roll, pitch: o.pitch, yaw: o.yaw), eulerMat(roll: a.roll, pitch: a.pitch, yaw: a.yaw)))
        if abs(o.pitch) < 1.5 { badOracleAng = max(badOracleAng, abs(o.roll - a.roll), abs(o.pitch - a.pitch), abs(o.yaw - a.yaw)) }
      } else { badOracleR = .infinity }
    }
    let a = a0
    P("ATT sample0 gravity=(\(f(d0.gravity.x, 4)),\(f(d0.gravity.y, 4)),\(f(d0.gravity.z, 4))) q=\(a.map { qs($0.quaternion) } ?? "nil") roll=\(a.map { f($0.roll, 4) } ?? "nil") pitch=\(a.map { f($0.pitch, 4) } ?? "nil") yaw=\(a.map { f($0.yaw, 4) } ?? "nil") R=\(a.map { matOf($0.rotationMatrix).map { $0.map { f($0, 3) }.joined(separator: " ") }.joined(separator: "; ") } ?? "nil") class=\(a.map { String(describing: type(of: $0)) } ?? "nil") desc='\(a?.description ?? "nil")'")
    if let a, let o = appleAttitude(a.quaternion) { P("ATT oracle q=\(qs(o.quaternion)) roll=\(f(o.roll, 4)) pitch=\(f(o.pitch, 4)) yaw=\(f(o.yaw, 4)) class=\(type(of: o))") }
    R(nilAtt == 0, "attitude.nonnil", "\(samples.count - nilAtt)/\(samples.count) samples have an attitude")
    R(badNorm < 1e-9, "quaternion.unit", "max ||q|-1|=\(badNorm)")
    R(badQ < 1e-9, "matrix.quaternion", "max |R - matrix(q)|=\(badQ)")
    R(badE < 1e-9, "matrix.euler", "max |R - matrix(roll,pitch,yaw)|=\(badE)")
    R(badG < 1e-3, "matrix.gravity", "max |R*(0,0,-1) - gravity|=\(badG)")
    R(badW < 1e-3, "gravity.\(expect)", "max |gravity - \(want)|=\(badW)")
    R(drift < 1e-6, "stable.2s", "max change over \(samples.count) samples=\(drift)")
    R(badMag == 0, "magneticField", "zero and uncalibrated in \(samples.count - badMag)/\(samples.count); heading=\(d0.heading)")
    R(badOracleR < 1e-9, "oracle.matrix", "max |R - Apple's rotationMatrix of the same q|=\(badOracleR)")
    R(badOracleE < 1e-9, "oracle.euler", "max |matrix(our angles) - matrix(Apple's angles)|=\(badOracleE); raw angle diff off gimbal lock=\(badOracleAng)")
    R(badOracleAng < 1e-9, "oracle.angles", "max raw angle diff where |pitch| < 1.5=\(badOracleAng)")
    if let a {
      R(((d0 as NSObject).value(forKey: "attitude") as AnyObject) === (a as AnyObject), "attitude.same", "one object per sample (as CoreMotion)")
      // multiplyByInverseOfAttitude: against Apple's on the same inputs (reference: a 30/20/10 degree attitude)
      let refQ = { () -> CMQuaternion in let (r, p, y) = (10.0 * .pi / 180, 20.0 * .pi / 180, 30.0 * .pi / 180)
        func m(_ a: CMQuaternion, _ b: CMQuaternion) -> CMQuaternion { CMQuaternion(x: a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y, y: a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x, z: a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w, w: a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z) }
        return m(CMQuaternion(x: 0, y: 0, z: sin(y / 2), w: cos(y / 2)), m(CMQuaternion(x: sin(p / 2), y: 0, z: 0, w: cos(p / 2)), CMQuaternion(x: 0, y: sin(r / 2), z: 0, w: cos(r / 2)))) }()
      let q0 = a.quaternion   // saved before the copy's multiply (the copy check compares the sample's attitude with it)
      if let ref = appleAttitude(refQ), let mine = a.copy() as? CMAttitude, let theirs = appleAttitude(a.quaternion) {
        mine.multiply(byInverseOf: ref); theirs.multiply(byInverseOf: ref)
        let dq = qdiff(mine.quaternion, theirs.quaternion), dr = mdiff(matOf(mine.rotationMatrix), matOf(theirs.rotationMatrix))
        let de = max(abs(mine.roll - theirs.roll), abs(mine.pitch - theirs.pitch), abs(mine.yaw - theirs.yaw))
        // Apple's matrix after its multiply carries float rounding (measured 3.1e-8 on 26.3; its quaternion agrees to 1e-17)
        R(dq < 1e-9 && dr < 1e-6 && de < 1e-9, "multiply.oracle", "ours q=\(qs(mine.quaternion)) Apple's q=\(qs(theirs.quaternion)) |dq|=\(dq) |dR|=\(dr) |dangles|=\(de)")
        R(qdiff(a.quaternion, q0) == 0 && qdiff(mine.quaternion, q0) > 1e-3 && (mine as AnyObject) !== (a as AnyObject), "copy", "copy is a new object; the copy's multiply changed it (|dq|=\(qdiff(mine.quaternion, q0))) and left the sample's attitude as before (|dq|=\(qdiff(a.quaternion, q0)))")
        let self1 = a.copy() as! CMAttitude; self1.multiply(byInverseOf: a)
        R(abs(abs(self1.quaternion.w) - 1) < 1e-12 && abs(self1.roll) + abs(self1.pitch) + abs(self1.yaw) < 1e-9, "multiply.self", "a * a^-1 = identity: q=\(qs(self1.quaternion))")
      } else { R(false, "multiply.oracle", "no oracle CMAttitude") }
      // coding: the archive must be Apple's own for the same q (class name CMAttitude, CoreMotion's keys),
      // so an app decodes a saved attitude without the engine; it decodes to a real CMAttitude with the same quaternion
      let data = try? NSKeyedArchiver.archivedData(withRootObject: a, requiringSecureCoding: true)
      let appleData = appleAttitude(a.quaternion).flatMap { try? NSKeyedArchiver.archivedData(withRootObject: $0, requiringSecureCoding: true) }
      let names = data.map { d in ["M5Attitude", "CMAttitude", "kCMAttitudeCodingKeyQW"].filter { d.range(of: Data($0.utf8)) != nil } } ?? []
      R(data != nil && data == appleData, "coding.apple", "archive \(data?.count ?? -1) bytes, Apple's for the same q \(appleData?.count ?? -1) bytes, identical=\(data != nil && data == appleData); names in it: \(names)")
      let back = data.flatMap { try? NSKeyedUnarchiver.unarchivedObject(ofClass: CMAttitude.self, from: $0) }
      // with the stand-in active the engine maps the name CMAttitude to M5Attitude for every decode (M1, below)
      R(back.map { NSStringFromClass(type(of: $0)) == "M5Attitude" && qdiff($0.quaternion, a.quaternion) == 0 && a.isEqual($0) } ?? false, "coding.decode",
        "decoded class=\(back.map { String(describing: type(of: $0)) } ?? "nil") q=\(back.map { qs($0.quaternion) } ?? "nil") |dq|=\(back.map { qdiff($0.quaternion, a.quaternion) } ?? -1)")
      m1(a)
    }
  }
  // M1: an attitude decoded from an archive of the stand-in, used with the stand-in as the ARGUMENT
  // (Apple's methods may read the argument's private storage, which an M5Attitude doesn't have). -m1 a|b|c runs one step
  // only, then ends (a throwaway launch: a crash ends the log after "M1 <step> begin"); no -m1: all three, asserted.
  func m1(_ a: CMAttitude) {
    let only = defs.string(forKey: "m1")
    func arch(_ x: CMAttitude) -> Data? { try? NSKeyedArchiver.archivedData(withRootObject: x, requiringSecureCoding: true) }
    func dec(_ d: Data?) -> CMAttitude? { d.flatMap { try? NSKeyedUnarchiver.unarchivedObject(ofClass: CMAttitude.self, from: $0) } }
    func cls(_ x: AnyObject?) -> String { x.map { String(describing: type(of: $0)) } ?? "nil" }
    // a second stand-in attitude, not equal to a: a copy of a turned by 30 degrees about z (its multiply is ours)
    guard let other = a.copy() as? CMAttitude, let turn = appleAttitude(CMQuaternion(x: 0, y: 0, z: sin(.pi / 12), w: cos(.pi / 12))) else { R(false, "m1.setup", "no copy/oracle"); return }
    other.multiply(byInverseOf: turn)
    if only == nil || only == "a" {   // decoded(other).multiply(byInverseOf: a) against Apple's on Apple's objects
      P("ATT M1 a begin")
      let dx = dec(arch(other)), want = appleAttitude(other.quaternion), wantArg = appleAttitude(a.quaternion)
      if let dx, let want, let wantArg {
        dx.multiply(byInverseOf: a); want.multiply(byInverseOf: wantArg)
        let dq = qdiff(dx.quaternion, want.quaternion), n = sqrt(dx.quaternion.x * dx.quaternion.x + dx.quaternion.y * dx.quaternion.y + dx.quaternion.z * dx.quaternion.z + dx.quaternion.w * dx.quaternion.w)
        P("ATT M1 a done decoded class=\(cls(dx)) result q=\(qs(dx.quaternion)) |q|=\(f(n, 6)) Apple's on Apple's q=\(qs(want.quaternion)) |dq|=\(dq)")
        R(dq < 1e-9, "m1.multiply", "decoded(\(cls(dx))).multiply(byInverseOf: dm.attitude) q=\(qs(dx.quaternion)) Apple's q=\(qs(want.quaternion)) |dq|=\(dq)")
      } else { R(false, "m1.multiply", "decode=\(cls(dx)) oracle=\(want != nil)") }
    }
    if only == nil || only == "b" {   // decoded(a).isEqual(a): the same quaternion, so YES; decoded(other) vs a: NO
      P("ATT M1 b begin")
      let da = dec(arch(a)), dx = dec(arch(other))
      let eq = da?.isEqual(a) ?? false, ne = dx?.isEqual(a) ?? true, rev = a.isEqual(da), h = (da?.hash ?? 0) == a.hash
      P("ATT M1 b done decoded class=\(cls(da)) decoded(a).isEqual(a)=\(eq) decoded(other).isEqual(a)=\(ne) a.isEqual(decoded(a))=\(rev) hash equal=\(h)")
      R(eq && !ne && rev && h, "m1.isEqual", "decoded(\(cls(da))).isEqual(dm.attitude)=\(eq), decoded(other).isEqual=\(ne), reverse=\(rev), hash equal=\(h)")
    }
    if only == nil || only == "c" {   // the class a secure decode returns, and that it carries the quaternion
      P("ATT M1 c begin")
      let da = dec(arch(a)), apple = appleAttitude(a.quaternion), dApple = apple.flatMap { dec(arch($0)) }
      P("ATT M1 c done unarchivedObject(ofClass: CMAttitude.self) of the stand-in's archive: \(cls(da)) q=\(da.map { qs($0.quaternion) } ?? "nil"); of Apple's: \(cls(dApple))")
      R(da != nil && cls(da) == "M5Attitude" && cls(dApple) == "M5Attitude" && qdiff(da!.quaternion, a.quaternion) == 0 && abs(da!.pitch - a.pitch) < 1e-9 && abs(da!.roll - a.roll) < 1e-9,
        "m1.decodeClass", "secure decode of the stand-in's archive: \(cls(da)), of Apple's archive: \(cls(dApple)); q and angles as the sample's")
    }
    if only != nil { finish() }
  }
  func finish() { P("ATT SUMMARY pass=\(pass) fail=\(fail)"); P("END"); exit(0) }
}

final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
  var window: UIWindow?
  func scene(_ scene: UIScene, willConnectTo s: UISceneSession, options: UIScene.ConnectionOptions) {
    guard let ws = scene as? UIWindowScene else { return }
    let w = UIWindow(windowScene: ws); let vc = VC(); vc.view.backgroundColor = .systemTeal; w.rootViewController = vc; w.makeKeyAndVisible(); window = w
  }
}
final class AppDelegate: UIResponder, UIApplicationDelegate {
  func application(_ a: UIApplication, didFinishLaunchingWithOptions o: [UIApplication.LaunchOptionsKey: Any]?) -> Bool { true }
  func application(_ a: UIApplication, configurationForConnecting s: UISceneSession, options: UIScene.ConnectionOptions) -> UISceneConfiguration {
    let c = UISceneConfiguration(name: nil, sessionRole: s.role); c.delegateClass = SceneDelegate.self; return c
  }
}
UIApplicationMain(CommandLine.argc, CommandLine.unsafeArgv, nil, NSStringFromClass(AppDelegate.self))
