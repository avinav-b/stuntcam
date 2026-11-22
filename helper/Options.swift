// SPDX-License-Identifier: Apache-2.0
// Options.swift: the helper's command line.
//   --home PATH        $SIMCAM_HOME (absolute). It is part of the ring name and holds the lock, the pid file, the
//                      reader files and the log.
//   --camera UNIQUEID  The camera, by AVCaptureDevice.uniqueID. The default is the built-in camera, else the first
//                      external one, and never a Continuity Camera or Desk View. The default's uniqueID is written to
//                      webcam/default.
//   --synthetic        No camera and no permission request: a moving pattern with the latency stamp. Its uniqueID is
//                      "synthetic" unless --camera names one.
//   --size WxH         The largest capture format (default 1920x1080). It is also the synthetic frame size.
//   --fps N            The frame rate (default 30).
//   --stamp            Stamp camera frames too (for latency tests).
//   --close-after S    Close the camera S seconds after the last active reader (default 10).
//   --quit-after S     Quit S seconds after the last reader of any kind (default 600).
//   --sha SHA          The <sha> recorded in the pid file (default: the name of the bundle's parent directory).
//   --jitter-ms N      For tests, with --synthetic: each frame is published 0-N ms (uniform) after its capture time,
//                      as a camera's publish times wander.
//   --list PATH        Write the cameras to PATH and exit (Discovery.swift describes the file). It takes no lock,
//                      writes no pid file or default file and asks for no permission, because listing cameras needs
//                      none.
//   --test-hooks       For tests: SIGUSR1 and SIGUSR2 act as the camera's disconnect and reconnect notifications. The
//                      synthetic source stops and starts as an unplugged camera would.
import Foundation

// What the command line asked for. It is parsed once, before anything else runs, and never changes afterwards.
struct Options {
  // the bounds the values are clamped to: --size each side in 1...SCW_MAX_DIM, --fps 1...240, --jitter-ms 0...30
  static let maxFPS = 240.0
  static let maxJitterMs = 30.0
  // the defaults
  static let defaultMaxWidth: UInt32 = 1920
  static let defaultMaxHeight: UInt32 = 1080
  static let defaultFPS = 30.0
  static let defaultCloseAfterS = 10.0
  static let defaultQuitAfterS = 600.0

  var home = ""
  var camera: String?
  var synthetic = false
  var maxWidth = Options.defaultMaxWidth
  var maxHeight = Options.defaultMaxHeight
  var fps = Options.defaultFPS
  var stampFrames = false
  var closeAfterS = Options.defaultCloseAfterS
  var quitAfterS = Options.defaultQuitAfterS
  var sha: String?
  var testHooks = false
  var jitterMs = 0.0
  var listPath: String?

  // the capture format these options ask for
  var formatLimits: FormatLimits {
    FormatLimits(maxWidth: maxWidth, maxHeight: maxHeight, fps: fps)
  }

  // Unknown arguments are ignored. A malformed value keeps the default, or the bound it is clamped to. Trailing slashes
  // are trimmed from --home.
  static func parse(_ arguments: ArraySlice<String>) -> Options {
    var options = Options()
    var rest = arguments.makeIterator()
    while let argument = rest.next() {
      switch argument {
      case "--home":
        options.home = rest.next() ?? ""
      case "--camera":
        options.camera = rest.next()
      case "--synthetic":
        options.synthetic = true
      case "--size":
        options.parseSize(rest.next() ?? "")
      case "--fps":
        options.fps = min(maxFPS, max(1, Double(rest.next() ?? "") ?? defaultFPS))
      case "--stamp":
        options.stampFrames = true
      case "--close-after":
        options.closeAfterS = max(0, Double(rest.next() ?? "") ?? defaultCloseAfterS)
      case "--quit-after":
        options.quitAfterS = max(1, Double(rest.next() ?? "") ?? defaultQuitAfterS)
      case "--sha":
        options.sha = rest.next()
      case "--test-hooks":
        options.testHooks = true
      case "--jitter-ms":
        options.jitterMs = min(maxJitterMs, max(0, Double(rest.next() ?? "") ?? 0))
      case "--list":
        options.listPath = rest.next() ?? ""
      default:
        break
      }
    }

    while options.home.count > 1 && options.home.hasSuffix("/") {
      options.home.removeLast()
    }

    return options
  }

  // "WxH", each side in 1...SCW_MAX_DIM; anything else keeps the current size
  private mutating func parseSize(_ text: String) {
    let sides = text.split(separator: "x").compactMap { UInt32($0) }
    let allowed = 1...UInt32(SCW_MAX_DIM)
    if sides.count == 2, allowed.contains(sides[0]), allowed.contains(sides[1]) {
      maxWidth = sides[0]
      maxHeight = sides[1]
    }
  }
}
