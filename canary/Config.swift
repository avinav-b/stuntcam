// SPDX-License-Identifier: Apache-2.0
import Foundation

// The settings the canary reads from its environment. main.swift builds the one instance, `config`, at launch.
struct Config {
  // CANARY_BUDGET, in seconds (default 45). A normal run takes about 15 s, and one "never" wait takes up to 15 s more.
  // The budget leaves room for both, so a photo, movie or frame that never completes is a FAIL, not INCONCLUSIVE.
  // scripts/run.sh waits 60 s for the RESULT line.
  let budget: Double
  // The QR payloads the qr stage accepts.
  // The generated scene draws SIMCAM_QR_TEXT, with the default below. With SIMCAM_OFF_GENERATED_SCENE, the fixture
  // image carries M1B-QR-OK instead. CANARY_QR accepts only the one value it names.
  let expectedQRs: [String]
  // expectedQRs as the check line prints them
  let expectedQR: String

  static let defaultBudgetS = 45.0
  static let defaultFirstFrameLimitS = 5.0

  static func fromEnvironment() -> Config {
    let env = ProcessInfo.processInfo.environment
    let budget = Double(env["CANARY_BUDGET"] ?? "") ?? defaultBudgetS
    let qrDefaults = [env["SIMCAM_QR_TEXT"] ?? "https://simcam.dev/m12-qr", "M1B-QR-OK"]
    let expectedQRs = env["CANARY_QR"].map { [$0] } ?? qrDefaults
    return Config(budget: budget, expectedQRs: expectedQRs, expectedQR: expectedQRs.joined(separator: "|"))
  }

  // CANARY_FIRST_FRAME_S, in seconds (default 5): the time-to-first-frame bound. Read at each first-frame check.
  static func firstFrameLimit() -> Double {
    Double(ProcessInfo.processInfo.environment["CANARY_FIRST_FRAME_S"] ?? "") ?? defaultFirstFrameLimitS
  }
}
