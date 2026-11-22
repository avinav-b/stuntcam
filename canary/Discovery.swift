// SPDX-License-Identifier: Apache-2.0
// Discovery: the cameras a DiscoverySession lists, the default back camera, and the virtual devices of the 8-camera
// profile.
import AVFoundation

// the 8-camera profile: how many cameras it lists, and where its triple camera switches between constituents
let profileCameraCount = 8
let tripleSwitchOverZoomFactors: [Double] = [2, 8]

extension Canary {
  // the default back wide camera, which the later stages use; the run ends here without one
  func discoverDevices() -> AVCaptureDevice {
    verdict.stage = "discovery"
    let discovery = AVCaptureDevice.DiscoverySession(
      deviceTypes: [.builtInWideAngleCamera, .builtInTripleCamera, .builtInDualWideCamera, .builtInTrueDepthCamera],
      mediaType: .video, position: .unspecified)
    keptUntilExit.append(discovery)
    let devices = discovery.devices
    check(!devices.isEmpty, "DiscoverySession devices=\(devices.map { "\($0.localizedName)/\($0.position.rawValue)" })")
    guard let device = AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .back) else {
      check(false, "default back wide")
      finish()
    }

    check(NSStringFromClass(type(of: device)).contains("FigVideoDevice"), "genuine device class \(type(of: device))")
    check(AVCaptureDevice(uniqueID: device.uniqueID) === device, "device(uniqueID:) identity")
    check(device.formats.count >= 2, "formats=\(device.formats.count)")
    check(device.formats.allSatisfy { !$0.videoSupportedFrameRateRanges.isEmpty }, "every format has fps ranges")
    return device
  }

  // the virtual devices of the 8-camera profile (only when the profile has them)
  func checkVirtualDevices() {
    verdict.stage = "virtual-devices"
    let all = AVCaptureDevice.DiscoverySession(
      deviceTypes: [.builtInWideAngleCamera, .builtInUltraWideCamera, .builtInTelephotoCamera, .builtInDualWideCamera,
                    .builtInTripleCamera, .builtInTrueDepthCamera, .builtInLiDARDepthCamera],
      mediaType: .video, position: .unspecified).devices
    log("devices: \(all.map { "\($0.localizedName)" })")
    if let triple = all.first(where: { $0.deviceType == .builtInTripleCamera }) {
      let constituents = triple.constituentDevices.map { $0.deviceType }
      let tripleParts: [AVCaptureDevice.DeviceType] = [.builtInUltraWideCamera, .builtInWideAngleCamera,
                                                       .builtInTelephotoCamera]
      check(triple.isVirtualDevice && constituents == tripleParts,
            "Triple virtual, constituents \(constituents.map { $0.rawValue })")
      check(triple.virtualDeviceSwitchOverVideoZoomFactors.map { $0.doubleValue } == tripleSwitchOverZoomFactors,
            "Triple switch-over \(triple.virtualDeviceSwitchOverVideoZoomFactors)")
      let dualWide = all.first { $0.deviceType == .builtInDualWideCamera }
      check(dualWide?.constituentDevices.map { $0.deviceType } == [.builtInUltraWideCamera, .builtInWideAngleCamera],
            "DualWide constituents")
      check(all.count == profileCameraCount, "\(profileCameraCount) cameras listed (\(all.count))")
    } else {
      log("profile without virtual devices: skipped")
    }
  }
}
