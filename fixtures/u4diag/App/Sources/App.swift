// SPDX-License-Identifier: Apache-2.0
import UIKit
import AVFoundation
import MachO
import os

// log dir: $M11_OUT (put it in the boot env, e.g. SIMCTL_CHILD_M11_OUT=<dir> scripts/boot.sh <rt> stub), else /tmp/simcam-m11/app
let outDir = ProcessInfo.processInfo.environment["M11_OUT"] ?? "/tmp/simcam-m11/app"
let pid = ProcessInfo.processInfo.processIdentifier
let osl = Logger(subsystem: "dev.simcam.m11", category: "app")
func note(_ s: String) {
    try? FileManager.default.createDirectory(atPath: outDir, withIntermediateDirectories: true)
    let line = "\(String(format: "%.3f", Date().timeIntervalSince1970)) \(s)\n"
    let p = "\(outDir)/app.\(pid).log"
    if let h = FileHandle(forWritingAtPath: p) { h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); h.closeFile() }
    else { try? line.write(toFile: p, atomically: true, encoding: .utf8) }
}
func toolLibs() -> [String] {
    var libs: [String] = []
    for i in 0..<_dyld_image_count() { let n = String(cString: _dyld_get_image_name(i)); if ["libViewDebuggerSupport", "libMainThreadChecker", "libRPAC", "libLogRedirect", "libsimcamstub", "libprobe", "libm11", "libm6fig", "libXCTest", "XCTest.framework", ".xctest", "ServeSim", "proxy", "libclang_rt", "libBacktraceRecording", "libgmalloc", "libMallocStackLogging", "libu4mark", "libsimcamloader", "libsimcam.dylib", "libXCTestBundleInject", "libLiveViewDebugger", "libPerfC"].contains(where: { n.contains($0) }) { libs.append((n as NSString).lastPathComponent) } }
    return libs
}
func imagesReport() -> String {
    let root = ProcessInfo.processInfo.environment["DYLD_ROOT_PATH"] ?? "\u{1}"; var s = ""
    for i in 0..<_dyld_image_count() { let n = String(cString: _dyld_get_image_name(i)); let rel = n.hasPrefix(root) ? String(n.dropFirst(root.count)) : n; s += "\(_dyld_shared_cache_contains_path(rel) ? "C" : "-") \(n)\n" }
    return s
}
func hostPathTest() {
    for dir in ["\(outDir)/hp"] {   // only our own log dir: can the app write to a host path
        do {
            try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            let f = "\(dir)/w.\(pid).txt"; try "pid \(pid)".write(toFile: f, atomically: true, encoding: .utf8)
            let back = try String(contentsOfFile: f, encoding: .utf8)
            let cfg = (try? String(contentsOfFile: "\(dir)/config.txt", encoding: .utf8)) ?? "(no config.txt)"
            note("hostpath OK \(dir) readback=\(back == "pid \(pid)") config=\(cfg.trimmingCharacters(in: .whitespacesAndNewlines))")
        } catch { note("hostpath FAIL \(dir): \(error)") }
    }
}
func mode() -> String {
    let args = ProcessInfo.processInfo.arguments
    if let i = args.firstIndex(of: "-M11MODE"), i + 1 < args.count { return args[i + 1] }
    if let m = ProcessInfo.processInfo.environment["M11_MODE"] { return m }
    return (try? String(contentsOfFile: "\(outDir)/mode.txt", encoding: .utf8))?.trimmingCharacters(in: .whitespacesAndNewlines) ?? "plain"
}

final class Cam: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
    let session = AVCaptureSession(); let lk = NSLock(); var nframes = 0
    var frames: Int { lk.lock(); defer { lk.unlock() }; return nframes }   // read on main, written on "frames" (TSan-clean)
    func start(_ done: @escaping (String) -> Void) {
        let devs = AVCaptureDevice.DiscoverySession(deviceTypes: [.builtInWideAngleCamera], mediaType: .video, position: .unspecified).devices
        note("camera devices=\(devs.map { "\($0.localizedName)/\($0.position.rawValue)" })")
        guard let d = devs.first(where: { $0.position == .back }) ?? devs.first, let input = try? AVCaptureDeviceInput(device: d) else { note("camera: NO CAMERA"); done("nocamera"); return }
        session.beginConfiguration(); session.addInput(input)
        let out = AVCaptureVideoDataOutput(); out.setSampleBufferDelegate(self, queue: DispatchQueue(label: "frames")); session.addOutput(out)
        session.commitConfiguration()
        DispatchQueue.global().async { self.session.startRunning(); note("camera running=\(self.session.isRunning)") }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { note("camera frames after 2s = \(self.frames)"); done("frames=\(self.frames)") }
    }
    func captureOutput(_ o: AVCaptureOutput, didOutput sb: CMSampleBuffer, from c: AVCaptureConnection) {
        lk.lock(); nframes += 1; let n = nframes; lk.unlock()
        if n == 1 { note(String(format: "camera first frame %.1f ms after process start", (Date().timeIntervalSince1970 - procStart()) * 1000)) }
    }
}

var gLabel: UILabel?
final class SceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    func scene(_ scene: UIScene, willConnectTo session: UISceneSession, options: UIScene.ConnectionOptions) {
        guard let ws = scene as? UIWindowScene else { return }
        let w = UIWindow(windowScene: ws); let vc = UIViewController(); vc.view.backgroundColor = .systemTeal
        if let l = gLabel { vc.view.addSubview(l) }
        w.rootViewController = vc; w.makeKeyAndVisible(); window = w
    }
}
@main
class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?; let cam = Cam(); var label = UILabel()
    func application(_ application: UIApplication, didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        label.frame = CGRect(x: 20, y: 200, width: 360, height: 60); label.numberOfLines = 0; label.text = "starting"; label.accessibilityIdentifier = "status"
        gLabel = label
        let env = ProcessInfo.processInfo.environment; let m = mode()
        note("START pid=\(pid) mode=\(m) exe=\(CommandLine.arguments[0])")
        for k in env.keys.sorted() where k.hasPrefix("DYLD") || k.hasPrefix("MTC") || k.hasPrefix("PERFC") || k.hasPrefix("IDE_") || k.hasPrefix("SIMCAM") || k.hasPrefix("M11") || k.hasPrefix("SERVE_SIM") || k.hasPrefix("XC") || k.hasPrefix("__XPC_DYLD") || k == "OS_ACTIVITY_DT_MODE" || ["NSZombie", "Malloc", "ASAN", "TSAN", "UBSAN", "__XCODE", "SIMULATOR_ROOT", "LIBGMALLOC", "IDB", "DETOX", "NSDeallocate", "NSDebug"].contains(where: { k.hasPrefix($0) }) { note("env \(k)=\(env[k]!)") }
        note("loaded tool libs: \(toolLibs())")
        note("diag: \(u4Diag())")
        var info = kinfo_proc(); var size = MemoryLayout<kinfo_proc>.stride; var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
        sysctl(&mib, 4, &info, &size, nil, 0); note("P_TRACED=\((info.kp_proc.p_flag & P_TRACED) != 0) arch=\(archName())")
        if m.contains("images") { try? imagesReport().write(toFile: "\(outDir)/images.\(pid).txt", atomically: true, encoding: .utf8) }
        hostPathTest()
        // console markers (Q1c)
        print("M11-PRINT pid=\(pid)"); NSLog("M11-NSLOG pid=%d", pid); osl.notice("M11-OSLOG pid=\(pid, privacy: .public)")
        fputs("M11-STDERR pid=\(pid)\n", stderr)
        if m.contains("camera") { cam.start { r in self.label.text = "done \(r)"; note("DONE \(r)") } } else { label.text = "done nocam"; note("DONE nocam") }
        if m.contains("crash") { DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { note("about to crash"); let p = UnsafeMutablePointer<Int>(bitPattern: 0x10)!; p.pointee = 1 } }
        if m.contains("abort") { DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { note("about to abort"); abort() } }
        if m.contains("exit0") { DispatchQueue.main.asyncAfter(deadline: .now() + 2.5) { note("about to exit(0)"); exit(0) } }
        return true
    }
}
func procStart() -> Double {   // kinfo_proc.p_starttime, as the engine's TIMING lines use
    var info = kinfo_proc(); var size = MemoryLayout<kinfo_proc>.stride; var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()]
    sysctl(&mib, 4, &info, &size, nil, 0); return Double(info.kp_proc.p_starttime.tv_sec) + Double(info.kp_proc.p_starttime.tv_usec) / 1e6
}
func archName() -> String {
    #if arch(x86_64)
    return "x86_64"
    #else
    return "arm64"
    #endif
}

// what the scheme's diagnostics actually do in this process
func u4Diag() -> String {
    let h = UnsafeMutableRawPointer(bitPattern: -2)   // RTLD_DEFAULT
    func sym(_ n: String) -> Bool { dlsym(h, n) != nil }
    var r = "asan=\(sym("__asan_init")) tsan=\(sym("__tsan_init")) ubsan=\(sym("__ubsan_handle_add_overflow"))"
    let env = ProcessInfo.processInfo.environment
    if env["NSZombieEnabled"] == "YES" {   // only then: a released object's memory is kept and its class becomes _NSZombie_<cls>
        // read the class through a C call on the raw pointer: no Swift retain of the dead object (a retain would trip the zombie)
        typealias GCN = @convention(c) (UnsafeRawPointer) -> UnsafePointer<CChar>
        let gcn = unsafeBitCast(dlsym(h, "object_getClassName")!, to: GCN.self)
        let o = Unmanaged.passRetained(NSObject()); let p = UnsafeRawPointer(o.toOpaque()); o.release()
        r += " zombie=\(String(cString: gcn(p)))"
    } else { r += " zombie=off" }
    r += " msl=\(env["MallocStackLogging"] ?? "-")"
    return r
}
