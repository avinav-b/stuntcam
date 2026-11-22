// SPDX-License-Identifier: Apache-2.0
// Discovery.swift: finding the Mac's cameras, choosing the one to publish, and --list.
import AVFoundation

// every camera the helper can publish: built-in, external, Continuity Camera and Desk View
func discoverCameras() -> [AVCaptureDevice] {
  AVCaptureDevice.DiscoverySession(
    deviceTypes: [.builtInWideAngleCamera, .external, .continuityCamera, .deskViewCamera],
    mediaType: .video, position: .unspecified).devices
}

// A Continuity Camera or Desk View is used only when --camera names it.
func isDefaultCandidate(_ device: AVCaptureDevice) -> Bool {
  !device.isContinuityCamera && device.deviceType != .deskViewCamera
}

// the camera used without --camera: the built-in camera, else the first external one
func defaultCamera(_ cameras: [AVCaptureDevice]) -> AVCaptureDevice? {
  cameras.first { isDefaultCandidate($0) && $0.deviceType == .builtInWideAngleCamera }
    ?? cameras.first { isDefaultCandidate($0) && $0.deviceType == .external }
}

// --list PATH writes the camera list and exits: 0 when it was written, 1 when not. The file's first line is
// "stuntcam-cameras 1", then there is one line per camera with tab-separated fields: the uniqueID, 1 if it is the
// camera chosen without --camera (else 0), 1 if it is a Continuity Camera (else 0), the device type and the localized
// name. It is written through PATH.tmp and a rename.
func listCameras(to path: String) -> Never {
  guard path.hasPrefix("/") else { fail("--list needs an absolute path", code: 2) }
  let cameras = discoverCameras()
  let chosen = defaultCamera(cameras)
  var text = "stuntcam-cameras 1\n"
  for camera in cameras {
    let isDefault = camera.uniqueID == chosen?.uniqueID ? 1 : 0
    let isContinuity = camera.isContinuityCamera ? 1 : 0
    text += "\(listField(camera.uniqueID))\t\(isDefault)\t\(isContinuity)\t"
      + "\(listField(camera.deviceType.rawValue))\t\(listField(camera.localizedName))\n"
  }
  exit(sch_write_file(path, text, strlen(text)) == 0 ? 0 : 1)
}

// A field of the camera list, with control characters turned into spaces so that each camera stays on one line and
// the tabs stay separators.
func listField(_ text: String) -> String {
  String(text.unicodeScalars.map { isControl($0) ? " " : Character($0) })
}

// the C0 control characters and DEL
func isControl(_ scalar: Unicode.Scalar) -> Bool {
  scalar.value < 0x20 || scalar.value == 0x7f
}

// The camera this helper publishes, chosen at launch. The synthetic source has no device.
struct CameraChoice {
  var device: AVCaptureDevice?
  var uniqueID = ""
  var name = "Synthetic"
  // the discovered cameras, logged once the log is open
  var discoveryLog: [String] = []

  // --camera by uniqueID, else the default camera. The run ends here when there is no camera at all.
  static func choose(_ options: Options) -> CameraChoice {
    var choice = CameraChoice()
    if options.synthetic {
      choice.uniqueID = options.camera ?? "synthetic"
      return choice
    }

    let cameras = discoverCameras()
    for camera in cameras {
      choice.discoveryLog.append("camera '\(camera.localizedName)' type=\(camera.deviceType.rawValue)"
                                 + " id=\(camera.uniqueID) continuity=\(camera.isContinuityCamera)"
                                 + " default-candidate=\(isDefaultCandidate(camera))")
    }

    if let uniqueID = options.camera {
      choice.uniqueID = uniqueID
      choice.device = cameras.first { $0.uniqueID == uniqueID } ?? AVCaptureDevice(uniqueID: uniqueID)
    } else {
      choice.device = defaultCamera(cameras)
      guard let device = choice.device else { fail("no camera found", code: 1) }
      choice.uniqueID = device.uniqueID
    }

    if let device = choice.device {
      choice.name = device.localizedName
    }

    return choice
  }
}
