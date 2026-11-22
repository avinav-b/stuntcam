// SPDX-License-Identifier: Apache-2.0
// Readers.swift: the reader files that engines write, the reader scan that opens and closes capture as readers come
// and go, and the liveness check that restarts capture when frames stop.
import Foundation

// A reader file, $SIMCAM_HOME/webcam/readers/<udid>-<pid> (docs/webcam-ring.md, "Reader files"): rings=, active= and
// want= lines.
struct ReaderFile {
  // a reader file is read into a buffer of this many bytes
  static let bufferLen = 512

  // the ring versions the engine accepts
  var rings: [UInt32] = []
  // whether the engine's capture session is running
  var active = false
  // the engine's active format, as the log prints it
  var want = "-"

  // The file at path, or nil if it can't be read. Unknown and malformed lines are skipped.
  static func read(_ path: String) -> ReaderFile? {
    var buffer = [CChar](repeating: 0, count: bufferLen)
    guard sch_read_small(path, &buffer, bufferLen) >= 0 else { return nil }
    var file = ReaderFile()
    for line in String(cString: buffer).split(separator: "\n") {
      let keyValue = line.split(separator: "=", maxSplits: 1).map(String.init)
      guard keyValue.count == 2 else { continue }
      let value = keyValue[1]
      switch keyValue[0] {
      case "rings": file.rings = value.split(separator: ",").compactMap { UInt32($0) }
      case "active": file.active = value == "1"
      case "want": file.want = value
      default: break
      }
    }

    return file
  }

  // whether the engine reads the ring version this helper publishes
  var readsOurRing: Bool { rings.contains(RingWriter.version) }
}

// what one reader scan found
struct ReaderScan {
  // a live reader of any kind
  var any = false
  // a live reader whose capture session runs and that reads this helper's ring version
  var active = false
  // each file's name and what it said, for the log
  var list: [String] = []
}

// seconds from start to end on the host clock (negative if end is earlier)
func seconds(from start: UInt64, to end: UInt64) -> Double {
  Double(Int64(bitPattern: end &- start)) / 1e9
}

extension Lifecycle {
  // a reader file whose process is gone is removed once it is this old (seconds)
  static let deadReaderUnlinkS = 5.0
  // Live engines rewrite their reader file every second, so one older than this (either way, in seconds) doesn't
  // count.
  static let readerStaleS = 3.0
  // Capture restarts when no frame came for this long while a reader is active. It is whole seconds, because the log
  // prints it.
  static let livenessTimeoutS = 2

  // The reader files whose process is alive and that are fresh. Files whose process is gone are removed once they are
  // old enough.
  func scanReaders() -> ReaderScan {
    var scan = ReaderScan()
    let names = (try? FileManager.default.contentsOfDirectory(atPath: readersDir)) ?? []  // no dir: no readers
    for name in names where !name.hasSuffix(".tmp") {
      guard let dash = name.lastIndex(of: "-"), let pid = Int32(name[name.index(after: dash)...]) else { continue }
      let path = readersDir + "/" + name
      let age = sch_age_s(path)
      guard sch_pid_alive(pid) != 0 else {
        if age > Self.deadReaderUnlinkS {
          unlink(path)
        }

        continue
      }

      // An older file (an app stopped in a debugger, or a reused pid) doesn't count.
      if age > Self.readerStaleS || age < -Self.readerStaleS {
        scan.list.append("\(name)(stale)")
        continue
      }

      guard let file = ReaderFile.read(path) else { continue }
      scan.any = true
      if file.active && file.readsOurRing {
        scan.active = true
      }

      let unsupported = file.readsOurRing ? "" : " rings=\(file.rings) unsupported"
      scan.list.append("\(name)(active=\(file.active ? 1 : 0) want=\(file.want)\(unsupported))")
    }

    return scan
  }

  // The reader scan: opens or closes capture as readers come and go, quits when none is left, and checks liveness.
  func tick() {
    if quitting { return }
    let now = sch_now_ns()
    let scan = scanReaders()
    let description = scan.list.sorted().joined(separator: " ")
    if description != readersLogged {
      readersLogged = description
      log("readers: \(description.isEmpty ? "none" : description)")
    }

    if scan.any {
      lastReaderNs = now
    }

    if scan.active {
      lastActiveNs = now
    }

    if scan.active && !wantOpen && canCapture {
      openCapture()
    } else if !scan.active && wantOpen && seconds(from: lastActiveNs, to: now) >= options.closeAfterS {
      closeCapture("no active reader for \(options.closeAfterS) s")
    }

    if !scan.any && seconds(from: lastReaderNs, to: now) >= options.quitAfterS {
      quit("no reader for \(options.quitAfterS) s", code: 0)
      return
    }

    checkLiveness()
    ring.queue.async { self.ring.logStatsAndReset() }
  }

  // Frames must keep coming while a reader is active and the camera is connected.
  func checkLiveness() {
    let timeout = Double(Self.livenessTimeoutS)
    guard wantOpen && !disconnected && seconds(from: openSince, to: sch_now_ns()) > timeout else { return }
    let lastFrameNs = ring.queue.sync { ring.lastFrameNs }
    if lastFrameNs < openSince || seconds(from: lastFrameNs, to: sch_now_ns()) > timeout {
      restartCapture("no frame for \(Self.livenessTimeoutS) s")
    }
  }
}
