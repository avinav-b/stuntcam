// SPDX-License-Identifier: Apache-2.0
// An extended check of the engine's operation trace (`stuntcam trace`, engine/src/core/trace.inc). The trace must
// have each photo taken in the photo stages, with its stages in order and non-decreasing host times, and with a
// timestamp op equal to the AVCapturePhoto.timestamp that was delivered.
//
// The trace file is trace-<pid>.txt in $SIMCAM_STATE_DIR/<bundle>/, or else in <container tmp>/simcam-state/<bundle>/.
import Foundation

// The fields of a trace file's op line, "op <index> <wall ns> <mach> <name> <settings id> <arg>", by position.
enum TraceField {
  static let count = 7
  static let wall = 2
  static let mach = 3
  static let name = 4
  static let settingsID = 5
  static let arg = 6
}

// how long the engine may take to write the trace file (it writes it about 150 ms after an op)
let traceWaitS = 2.0

// one op line of a trace file
struct TraceOp {
  let name: String
  let mach: UInt64
  let settingsID: Int64
  let arg: Int64
  let wall: UInt64
}

// the ops each photo must have, in this order. slot12 is the engine's capture entry (Fig slot 12,
// IrisStillImageSinkCaptureImage: AVFoundation hands the request to the session).
let tracePhotoStages = ["shutter", "slot12", "will-capture", "timestamp", "did-capture", "encode-start", "encoded",
                        "still-complete", "did-finish"]
// the timestamp op is within this many ms after slot12 (the timestamp is the capture moment)
let timestampAfterSlot12MaxMs = 50.0
// the EXIF capture time is within this many ms of the timestamp op's wall time
let exifToTimestampMaxMs = 2.0
// the timestamp op and AVCapturePhoto.timestamp are the same instant if they differ by less than 1 µs
let sameInstantToleranceS = 1e-6

// The trace's wall times and the timestamp op's arg are in ns; its mach times are host ticks.
extension mach_timebase_info_data_t {
  // host (mach) ticks in nanoseconds, by this timebase
  func nanoseconds(_ ticks: Double) -> Double {
    ticks * Double(numer) / Double(denom)
  }
}

// the op lines of a trace file; other lines are skipped
func parseTraceOps(_ text: String) -> [TraceOp] {
  var ops: [TraceOp] = []
  for line in text.split(separator: "\n") {
    let fields = line.split(separator: " ")
    guard fields.count == TraceField.count, fields[0] == "op", let wall = UInt64(fields[TraceField.wall]),
          let mach = UInt64(fields[TraceField.mach]), let settingsID = Int64(fields[TraceField.settingsID]),
          let arg = Int64(fields[TraceField.arg]) else { continue }
    ops.append(TraceOp(name: String(fields[TraceField.name]), mach: mach, settingsID: settingsID, arg: arg, wall: wall))
  }
  return ops
}

extension Canary {
  func traceCheck() {
    verdict.stage = "trace"
    let want = tracePhotos
    guard !want.isEmpty else {
      check(false, "photos to look for in the trace", ext: true)
      return
    }

    let bundle = Bundle.main.bundleIdentifier ?? "-"
    let name = "trace-\(getpid()).txt"
    var dirs: [String] = []
    if let stateDir = ProcessInfo.processInfo.environment["SIMCAM_STATE_DIR"], !stateDir.isEmpty {
      dirs.append("\(stateDir)/\(bundle)")
    }

    dirs.append((NSTemporaryDirectory() as NSString).appendingPathComponent("simcam-state/\(bundle)"))

    // the last complete file read (one that ends with "end"), and its ops
    var path = ""
    var ops: [TraceOp] = []

    // Reads the first complete trace file. It returns true once the file has every photo's did-finish op, which is
    // the last stage.
    func have() -> Bool {
      for dir in dirs {
        let candidate = "\(dir)/\(name)"
        guard let text = try? String(contentsOfFile: candidate, encoding: .utf8), text.hasSuffix("end\n") else {
          continue
        }

        path = candidate
        ops = parseTraceOps(text)
        return want.allSatisfy { photo in
          ops.contains { $0.settingsID == photo.settingsID && $0.name == "did-finish" }
        }
      }

      return false
    }

    let found = waitFor(traceWaitS) { have() }
    check(!path.isEmpty, "trace file \(name) in \(dirs)", ext: true)
    guard !path.isEmpty else { return }
    check(found, "trace has did-finish for photos \(want.map { $0.settingsID }) (\(ops.count) ops)", ext: true)
    var timebase = mach_timebase_info_data_t()
    mach_timebase_info(&timebase)
    for photo in want {
      let photoOps = ops.filter { $0.settingsID == photo.settingsID && tracePhotoStages.contains($0.name) }
      checkTracedPhoto(photo, photoOps, timebase)
    }
  }

  // one photo's ops: the stages in order, host times non-decreasing, the timestamp op equal to the delivered
  // timestamp and close after slot12, and the EXIF capture time equal to the timestamp's wall time
  func checkTracedPhoto(_ photo: (settingsID: Int64, timestamp: Double, exif: Double?), _ ops: [TraceOp],
                        _ timebase: mach_timebase_info_data_t) {
    let names = ops.map { $0.name }
    check(names == tracePhotoStages, "photo \(photo.settingsID) stages in order \(names)", ext: true)
    check(zip(ops, ops.dropFirst()).allSatisfy { $0.mach <= $1.mach },
          "photo \(photo.settingsID) host times non-decreasing",
          ext: true)
    guard let timestampOp = ops.first(where: { $0.name == "timestamp" }) else { return }
    let argS = Double(timestampOp.arg) / nanosecondsPerSecond
    let viaMachS = timebase.nanoseconds(Double(timestampOp.mach)) / nanosecondsPerSecond
    let argIsTimestamp = abs(argS - photo.timestamp) < sameInstantToleranceS
    let machIsTimestamp = abs(viaMachS - photo.timestamp) < sameInstantToleranceS
    check(argIsTimestamp && machIsTimestamp,
          "photo \(photo.settingsID) timestamp op \(timestampOp.arg) ns"
            + " == AVCapturePhoto.timestamp \(String(format: "%.9f", photo.timestamp))",
          ext: true)
    guard let slot12Op = ops.first(where: { $0.name == "slot12" }) else { return }
    let afterSlot12Ms = timebase.nanoseconds(Double(timestampOp.mach) - Double(slot12Op.mach))
      / nanosecondsPerMillisecond
    check(afterSlot12Ms >= 0 && afterSlot12Ms <= timestampAfterSlot12MaxMs,
          "photo \(photo.settingsID) timestamp - slot12 \(String(format: "%.1f", afterSlot12Ms)) ms"
            + " (0..\(Int(timestampAfterSlot12MaxMs)) ms)", ext: true)
    // The EXIF capture time (DateTimeOriginal, SubsecTimeOriginal and OffsetTimeOriginal, rounded to the ms) is the
    // timestamp's instant, the frame grab, on the wall clock. The timestamp op carries the same wall value: the engine
    // computes it once from the grab's mach time and uses it for both. It is not checked against slot12 with the 50 ms
    // window, because on 27.0 the whole encode takes 10-18 ms, so a stamp taken during the encode would fit it too.
    guard let exifSeconds = photo.exif else {
      check(false, "photo \(photo.settingsID) EXIF DateTimeOriginal with SubsecTimeOriginal + OffsetTimeOriginal",
            ext: true)
      return
    }

    let exifMinusTimestampMs = (exifSeconds - Double(timestampOp.wall) / nanosecondsPerSecond) * millisecondsPerSecond
    let exifMinusSlot12Ms = (exifSeconds - Double(slot12Op.wall) / nanosecondsPerSecond) * millisecondsPerSecond
    check(exifMinusTimestampMs >= -exifToTimestampMaxMs && exifMinusTimestampMs <= exifToTimestampMaxMs,
          "photo \(photo.settingsID) EXIF DateTimeOriginal - timestamp"
            + " \(String(format: "%.1f", exifMinusTimestampMs)) ms"
            + " (-\(Int(exifToTimestampMaxMs))..\(Int(exifToTimestampMaxMs)) ms;"
            + " - slot12 \(String(format: "%.1f", exifMinusSlot12Ms)) ms)",
          ext: true)
  }
}
