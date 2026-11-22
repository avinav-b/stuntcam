// SPDX-License-Identifier: Apache-2.0
// Hosted test: runs inside the host app (M11App) and records what the scheme's diagnostics put in this process,
// whether stuntcam's restore kept it, and whether the host app's camera ran. Writes $M11_OUT/unittest.<pid>.txt.
import XCTest
import MachO

final class HostedTests: XCTestCase {
    func diag(_ env: [String: String]) -> String {
        let h = UnsafeMutableRawPointer(bitPattern: -2)   // RTLD_DEFAULT
        func sym(_ n: String) -> Bool { dlsym(h, n) != nil }
        var r = "asan=\(sym("__asan_init")) tsan=\(sym("__tsan_init")) ubsan=\(sym("__ubsan_handle_add_overflow"))"
        if env["NSZombieEnabled"] == "YES" {   // only then: a released object's memory is kept and its class becomes _NSZombie_<cls>
            // read the class through a C call on the raw pointer: no Swift retain of the dead object (a retain would trip the zombie)
            typealias GCN = @convention(c) (UnsafeRawPointer) -> UnsafePointer<CChar>
            let gcn = unsafeBitCast(dlsym(h, "object_getClassName")!, to: GCN.self)
            let o = Unmanaged.passRetained(NSObject()); let p = UnsafeRawPointer(o.toOpaque()); o.release()
            r += " zombie=\(String(cString: gcn(p)))"
        } else { r += " zombie=off" }
        return r + " msl=\(env["MallocStackLogging"] ?? "-")"
    }
    func testHosted() throws {
        let pid = ProcessInfo.processInfo.processIdentifier
        let env = ProcessInfo.processInfo.environment
        let out = env["M11_OUT"] ?? "/tmp/simcam-m11/app"   // same log dir as the host app (App.swift)
        // the host app starts its camera at launch (mode.txt = camera) and logs "DONE frames=N" 2 s later
        var cam = "no DONE line within 8 s"
        let until = Date().addingTimeInterval(8)
        while Date() < until {
            if let s = try? String(contentsOfFile: "\(out)/app.\(pid).log", encoding: .utf8), let r = s.range(of: "DONE ") {
                cam = String(s[r.lowerBound...].prefix(while: { $0 != "\n" })); break
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        }
        let root = env["DYLD_ROOT_PATH"] ?? env["SIMULATOR_ROOT"] ?? "\u{1}"
        var libs: [String] = [], images = ""
        let names = ["libsimcam", "libprobe", "libXCTestBundleInject", "libMainThreadChecker", "libLogRedirect", "libRPAC", "libViewDebuggerSupport",
                     "libclang_rt", "libBacktraceRecording", "libgmalloc", "libMallocStackLogging", "libu4mark"]
        for i in 0..<_dyld_image_count() {
            let n = String(cString: _dyld_get_image_name(i))
            if names.contains(where: { n.contains($0) }) { libs.append((n as NSString).lastPathComponent) }
            let rel = n.hasPrefix(root) ? String(n.dropFirst(root.count)) : n
            if !_dyld_shared_cache_contains_path(rel) { images += "image \(n)\n" }   // everything not from the runtime's shared cache
        }
        var s = "hosted test pid=\(pid) exe=\(CommandLine.arguments[0])\nlibs=\(libs)\ndiag: \(diag(env))\ncamera: \(cam)\n"
        for k in env.keys.sorted() where ["DYLD", "XC", "SIMCAM", "M11", "MTC", "PERFC", "IDE_", "NSZombie", "Malloc", "ASAN", "TSAN", "UBSAN", "__XCODE", "SIMULATOR_ROOT", "LIBGMALLOC"].contains(where: { k.hasPrefix($0) }) { s += "env \(k)=\(env[k]!)\n" }
        s += images
        try? FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
        try s.write(toFile: "\(out)/unittest.\(pid).txt", atomically: true, encoding: .utf8)
        print("M11-UNITTEST-PRINT pid=\(pid) libs=\(libs) camera=\(cam)")
        XCTAssertNotNil(NSClassFromString("M11App.AppDelegate"), "running inside the host app")
    }
}
