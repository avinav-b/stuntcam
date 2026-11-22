// SPDX-License-Identifier: Apache-2.0
// Log.swift: the helper's log, $SIMCAM_HOME/logs/webcam-<cam>.log, and fail() for the errors that come before it is
// open.
import Foundation

// The log file, opened once at launch (main.swift) and appended to from any queue. Each line is
// "<unix time> [helper <pid>] <message>".
final class LogFile {
  private let fd: Int32
  private let lock = NSLock()

  init(path: String) {
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0o600)
  }

  func write(_ message: String) {
    let line = String(format: "%.3f [helper %d] ", Date().timeIntervalSince1970, getpid()) + message + "\n"
    lock.lock()
    defer { lock.unlock() }
    _ = line.withCString { Darwin.write(fd, $0, strlen($0)) }  // a failed log write has nowhere to be reported
  }
}

// one line in the log (main.swift opens it before the first call)
func log(_ message: String) {
  logFile.write(message)
}

// An error before the log is open: one line on stderr, then exit.
func fail(_ message: String, code: Int32) -> Never {
  FileHandle.standardError.write(Data("Stuntcam Camera: \(message)\n".utf8))
  exit(code)
}
