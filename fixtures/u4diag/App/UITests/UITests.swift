// SPDX-License-Identifier: Apache-2.0
import XCTest
final class M11UITests: XCTestCase {
    func waitDone(_ app: XCUIApplication, _ tag: String) {
        XCTAssertTrue(app.staticTexts["status"].waitForExistence(timeout: 15))
        let ok = NSPredicate(format: "label BEGINSWITH 'done'")
        expectation(for: ok, evaluatedWith: app.staticTexts["status"]); waitForExpectations(timeout: 15)
        print("M11-UITEST\(tag) status=\(app.staticTexts["status"].label)")
    }
    func testLaunch() throws {
        let app = XCUIApplication()
        app.launchEnvironment["M11_MARK"] = "from-launchEnvironment"
        app.launch()
        waitDone(app, "")
    }
    // a test that inserts its own library ($U4MARK_LIB in the runner's environment, TEST_RUNNER_U4MARK_LIB); both it and
    // stuntcam must load in the app
    func testOwnInsert() throws {
        let env = ProcessInfo.processInfo.environment
        guard let lib = env["U4MARK_LIB"], let out = env["U4MARK_OUT"] else { throw XCTSkip("U4MARK_LIB / U4MARK_OUT not set") }
        let app = XCUIApplication()
        app.launchEnvironment["DYLD_INSERT_LIBRARIES"] = lib
        app.launchEnvironment["U4MARK_OUT"] = out
        app.launch()
        waitDone(app, "-OWN")
        print("M11-UITEST-OWN launchEnvironment after launch: \(app.launchEnvironment["DYLD_INSERT_LIBRARIES"] ?? "-")")
    }
    // activate() on an app that isn't running launches it
    func testActivate() throws {
        let app = XCUIApplication()
        app.terminate()
        app.activate()
        waitDone(app, "-ACTIVATE")
    }
}
