// SPDX-License-Identifier: Apache-2.0
// ShutterTests — test-only UI test for the AVCam fixture (fixtures/avcam/test.sh). Not part of the sample or the patch.
// Launches the installed AVCam (bundle id com.example.apple-samplecode.AVCam), waits for the capture session (the
// feature toolbar appears only while it runs), reads the Live Photo toggle, then taps the shutter SHOTS times (default 20),
// waiting GAP seconds (default 2) after each tap and checking that the app is still in the foreground. Lines start with
// "SHUTTER|" so test.sh can grep them from the xcodebuild log. OUTDIR (a dir the simulator can write, under $SIMCAM_HOME)
// gets the element tree and screenshots. The test fails if the app is gone after a tap (it does not relaunch it).
import XCTest

final class ShutterTests: XCTestCase {
    private let env = ProcessInfo.processInfo.environment

    private func log(_ s: String) { print("SHUTTER| \(s)") }

    private func save(_ shot: XCUIScreenshot, _ name: String) {
        guard let dir = env["OUTDIR"], !dir.isEmpty else { return }
        try? shot.pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name))
    }

    func testShutter() throws {
        let shots = Int(env["SHOTS"] ?? "") ?? 20
        let gap = Double(env["GAP"] ?? "") ?? 2.0
        let app = XCUIApplication(bundleIdentifier: "com.example.apple-samplecode.AVCam")
        app.launch()
        log("launched state=\(app.state.rawValue)")

        // The Live Photo toggle (SF Symbol "livephoto" / "livephoto.slash") is in the feature toolbar, which AVCam shows
        // only once the capture session runs.
        let live = app.buttons.matching(NSPredicate(format: "label CONTAINS[c] 'live'")).firstMatch
        let running = live.waitForExistence(timeout: 30) && live.isHittable
        if let dir = env["OUTDIR"], !dir.isEmpty {
            try? app.debugDescription.write(toFile: dir + "/tree.txt", atomically: true, encoding: .utf8)
        }
        log("session running=\(running) livephoto-toggle label='\(live.exists ? live.label : "-")' id='\(live.exists ? live.identifier : "-")'")
        save(app.screenshot(), "before.png")
        XCTAssertTrue(running, "AVCam's feature toolbar never appeared (capture session not running)")

        // The shutter: AVCam's photo capture button, 58 pt wide (measured) and unlabelled; the thumbnail and camera-switch buttons
        // next to it are 48 pt.
        let window = app.windows.firstMatch.frame
        let shutter = app.buttons.allElementsBoundByIndex.first {
            $0.frame.width >= 56 && $0.frame.width <= 90 && $0.frame.midY > window.midY
        }
        guard let shutter else { XCTFail("no shutter button found"); return }
        log("shutter frame=\(shutter.frame)")

        var taken = 0
        for i in 1...shots {
            shutter.tap()
            Thread.sleep(forTimeInterval: gap)
            let state = app.state
            log("shot \(i) state=\(state.rawValue)")
            if state != .runningForeground { XCTFail("AVCam not in the foreground after shot \(i) (state \(state.rawValue))"); break }
            taken += 1
        }
        log("done taps=\(taken)/\(shots) livephoto-toggle label='\(live.exists ? live.label : "-")'")
        if app.state == .runningForeground { save(app.screenshot(), "after.png") }
    }
}
