// SPDX-License-Identifier: Apache-2.0
// Permission.swift: the camera permission, settled once at launch, before any reader exists.
import AVFoundation

// the permission as the pid file records it (SCW_AUTH_*)
func authCode(_ status: AVAuthorizationStatus) -> Int32 {
  switch status {
  case .authorized: return Int32(SCW_AUTH_AUTHORIZED)
  case .notDetermined: return Int32(SCW_AUTH_NOT_DETERMINED)
  default: return Int32(SCW_AUTH_DENIED)
  }
}

// the permission as the log prints it
func authName(_ status: AVAuthorizationStatus) -> String {
  switch status {
  case .authorized: return "authorized"
  case .notDetermined: return "notDetermined"
  case .denied: return "denied"
  default: return "restricted"
  }
}

extension Lifecycle {
  // When authorized, the ring goes idle. When not determined yet, the user is asked. When denied or restricted, the
  // helper exits. The synthetic source needs no permission, so for it this only reports the permission.
  func settlePermission() {
    let status = AVCaptureDevice.authorizationStatus(for: .video)
    pidFile.auth = authCode(status)
    pidFile.write()
    log("camera permission: \(authName(status))")
    if options.synthetic {
      ring.setState(RingState.idle)
      return
    }

    switch status {
    case .authorized: ring.setState(RingState.idle)
    case .notDetermined: requestAccess()
    default: denied()
    }
  }

  // Shows the permission dialog. Meanwhile the pid file and the ring say prompting.
  func requestAccess() {
    pidFile.auth = Int32(SCW_AUTH_PROMPTING)
    ring.setState(RingState.prompting)
    pidFile.write()
    log("requesting camera access (a dialog is shown)")
    AVCaptureDevice.requestAccess(for: .video) { granted in
      DispatchQueue.main.async {
        log("camera access \(granted ? "granted" : "denied")")
        if granted {
          self.pidFile.auth = Int32(SCW_AUTH_AUTHORIZED)
          self.pidFile.write()
          self.ring.setState(RingState.idle)
          self.tick()
        } else {
          self.denied()
        }
      }
    }
  }

  // The pid file stays, with state=denied, for the CLI to read. The ring holds no frame and says denied. The helper
  // exits deniedExitS seconds later.
  func denied() {
    pidFile.auth = Int32(SCW_AUTH_DENIED)
    ring.setState(RingState.denied)

    // waits until the ring has the denied state; its pid file update is then queued before the write below
    ring.queue.sync {}
    DispatchQueue.main.async { self.pidFile.write() }
    log("camera access denied or restricted: System Settings > Privacy & Security > Camera > Stuntcam Camera; exiting")
    DispatchQueue.main.asyncAfter(deadline: .now() + Self.deniedExitS) { exit(0) }
  }

  // whether capture may open: always for the synthetic source, else once the camera permission is granted
  var canCapture: Bool {
    options.synthetic || pidFile.auth == Int32(SCW_AUTH_AUTHORIZED)
  }
}
