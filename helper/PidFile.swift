// SPDX-License-Identifier: Apache-2.0
// PidFile.swift: the pid file, $SIMCAM_HOME/webcam/<cam>.pid (docs/webcam-ring.md, "The pid file"). The CLI reads the
// helper's pid, <sha>, ring versions, camera permission and ring state from it.
import Foundation

// The pid file's contents, rewritten whole (through a .tmp file and a rename) on every change.
// Threading: main queue only.
final class PidFile {
  // the formatted file is at most this many bytes
  static let bufferLen = 1024

  let path: String
  // the camera permission as the helper sees it (SCW_AUTH_*)
  var auth = Int32(SCW_AUTH_NOT_DETERMINED)
  // the ring state last written
  var state = RingState.starting
  private let sha: String
  private let uniqueID: String
  private let startedUnix: UInt64

  init(path: String, sha: String, uniqueID: String, startedUnix: UInt64) {
    self.path = path
    self.sha = sha
    self.uniqueID = uniqueID
    self.startedUnix = startedUnix
  }

  func write() {
    var record = scw_pidfile()
    sch_pidfile_fill(&record, getpid(), sha, auth, state, uniqueID, startedUnix)
    var buffer = [CChar](repeating: 0, count: Self.bufferLen)
    let length = scw_pidfile_format(&buffer, Self.bufferLen, &record)
    if length > 0 {
      _ = sch_write_file(path, buffer, Int(length))  // the next change writes it again
    } else {
      log("pid file: format failed (\(length))")
    }
  }

  // A new ring state: written and logged only when it differs from the one last written.
  func update(state newState: UInt32) {
    if state != newState {
      state = newState
      write()
      log("state \(RingState.name(newState))")
    }
  }
}
