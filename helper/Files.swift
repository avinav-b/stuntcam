// SPDX-License-Identifier: Apache-2.0
// Files.swift: where the helper's files are and what they are called. docs/webcam-ring.md describes each file:
// <cam>.lock (held for the helper's whole life) and <cam>.pid in "The pid file", default in "The default camera", and
// readers/ in "Reader files".
import Foundation

// the helper's directories under $SIMCAM_HOME
struct Directories {
  let webcam: String
  let readers: String
  let logs: String

  init(home: String) {
    webcam = home + "/webcam"
    readers = webcam + "/readers"
    logs = home + "/logs"
  }

  // mkdir's result is deliberately unused. EEXIST is the usual case, and a directory that can't be made shows up at
  // once: the lock fails (logged with its errno), or there is no log file.
  func create() {
    for directory in [webcam, readers, logs] {
      _ = mkdir(directory, 0o700)
    }
  }
}

// the names that come from the camera: its short name in file names, the ring's shm name, the pid and lock files
struct CameraFiles {
  // buffer sizes for the C helpers' outputs: the camera hash (8 hex digits and a NUL), the shm name
  static let camHashBufferLen = 9   // mirrors ring.h's SCW_CAM_HASH_BYTES: 8 hex digits and a NUL
  static let shmNameBufferLen = 64

  // a hash of $SIMCAM_HOME and the camera's uniqueID
  let cam: String
  let shmName: String
  let pidPath: String
  let lockPath: String

  init(home: String, uniqueID: String, webcamDir: String) {
    var hash = [CChar](repeating: 0, count: Self.camHashBufferLen)
    scw_cam_hash(home, uniqueID, &hash)
    cam = String(cString: hash)
    var name = [CChar](repeating: 0, count: Self.shmNameBufferLen)
    _ = scw_shm_name(&name, Self.shmNameBufferLen, UInt(getuid()), cam, RingWriter.version)  // can't fail: 8-hex hash
    shmName = String(cString: name)
    pidPath = webcamDir + "/\(cam).pid"
    lockPath = webcamDir + "/\(cam).lock"
  }
}

// The <sha> recorded in the pid file: --sha, else the name of the bundle's parent directory (helper/build.sh copies the
// bundle to $SIMCAM_HOME/helper/<sha>/). Anything but 12 lowercase hex digits becomes twelve zeros.
func bundleSHA(_ shaOption: String?) -> String {
  // a <sha> is the first 12 hex digits of the bundle's hash (helper/build.sh)
  let shaLength = 12
  let bundleURL = URL(fileURLWithPath: Bundle.main.bundlePath)
  let candidate = shaOption ?? bundleURL.deletingLastPathComponent().lastPathComponent
  let isSHA = candidate.count == shaLength && candidate.allSatisfy({ "0123456789abcdef".contains($0) })
  return isSHA ? candidate : String(repeating: "0", count: shaLength)
}

// webcam/default: the uniqueID of the camera chosen without --camera
func writeDefaultCamera(_ uniqueID: String, webcamDir: String) {
  let text = uniqueID + "\n"
  _ = sch_write_file(webcamDir + "/default", text, strlen(text))  // best effort: without it `webcam` finds no camera
}
