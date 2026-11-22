// SPDX-License-Identifier: Apache-2.0
// Extended device controls: focus and exposure metering, torch, zoom readback, exposure bias completion + readback.
import AVFoundation

// the point of interest metering is pointed at (normalized coordinates)
let meteringPoint = CGPoint(x: 0.3, y: 0.3)
// a zoom factor or exposure bias reads back within this of what was set
let zoomReadbackTolerance: CGFloat = 0.01
let biasReadbackTolerance: Float = 0.01

// how long focus and exposure metering may take to report adjusting and then done, the torch to report its state,
// and the exposure bias to call its completion handler
let focusCycleWaitS = 1.5
let exposureCycleWaitS = 0.5
let torchWaitS = 1.0
let biasCompletionWaitS = 1.0
// the zoom factor set (or the format's maximum, if lower)
let testZoomFactor: CGFloat = 2

// The values that key-value observation reported for the device's adjusting and torch flags, in order.
// They are appended under Canary.lock. The check messages print them after their wait, without the lock.
final class ControlChanges {
  var adjustingFocus: [Bool] = []
  var adjustingExposure: [Bool] = []
  var torchActive: [Bool] = []
}

extension Canary {
  func checkDeviceControls(_ device: AVCaptureDevice) {
    verdict.stage = "ext-controls"
    do {
      let changes = observeControls(device)
      try checkMetering(device, changes)
      try checkTorch(device, changes)
      try checkZoom(device)
      try checkExposureBias(device)
    } catch { check(false, "device controls threw \(error)", ext: true) }
  }

  // starts observing the device's adjusting and torch flags (the observers live until teardown)
  func observeControls(_ device: AVCaptureDevice) -> ControlChanges {
    let changes = ControlChanges()
    controlObservers.append(device.observe(\.isAdjustingFocus, options: [.new]) { _, change in
      self.lock.withLock { changes.adjustingFocus.append(change.newValue ?? false) }
    })

    controlObservers.append(device.observe(\.isAdjustingExposure, options: [.new]) { _, change in
      self.lock.withLock { changes.adjustingExposure.append(change.newValue ?? false) }
    })

    controlObservers.append(device.observe(\.isTorchActive, options: [.new]) { _, change in
      self.lock.withLock { changes.torchActive.append(change.newValue ?? false) }
    })

    return changes
  }

  // focus and exposure at a point of interest: each must report adjusting, then done
  func checkMetering(_ device: AVCaptureDevice, _ changes: ControlChanges) throws {
    try withConfiguration(device) {
      if device.isFocusPointOfInterestSupported {
        device.focusPointOfInterest = meteringPoint
        device.focusMode = .autoFocus
      }

      if device.isExposurePointOfInterestSupported {
        device.exposurePointOfInterest = meteringPoint
        device.exposureMode = .autoExpose
      }
    }

    // adjusting was reported, and the last value reported is done
    func cycled(_ values: () -> [Bool]) -> Bool {
      lock.withLock { values().contains(true) && values().last == false }
    }

    check(waitFor(focusCycleWaitS) { cycled { changes.adjustingFocus } },
          "focus metering cycles isAdjustingFocus YES->NO (\(changes.adjustingFocus))", ext: true)
    check(waitFor(exposureCycleWaitS) { cycled { changes.adjustingExposure } },
          "exposure metering cycles YES->NO (\(changes.adjustingExposure))", ext: true)
  }

  func checkTorch(_ device: AVCaptureDevice, _ changes: ControlChanges) throws {
    guard device.hasTorch else { return }
    try withConfiguration(device) { device.torchMode = .on }
    check(waitFor(torchWaitS) { device.isTorchActive }, "torch on -> isTorchActive (\(changes.torchActive))", ext: true)
    try withConfiguration(device) { device.torchMode = .off }
    check(waitFor(torchWaitS) { !device.isTorchActive }, "torch off -> !isTorchActive", ext: true)
  }

  // zoom to testZoomFactor (or the format's maximum) reads back, then back to 1x
  func checkZoom(_ device: AVCaptureDevice) throws {
    let zoom = min(testZoomFactor, device.activeFormat.videoMaxZoomFactor)
    try withConfiguration(device) { device.videoZoomFactor = zoom }
    check(abs(device.videoZoomFactor - zoom) < zoomReadbackTolerance,
          "zoom readback \(device.videoZoomFactor)", ext: true)
    try withConfiguration(device) { device.videoZoomFactor = 1 }
  }

  // Exposure bias must complete and read back (a renamed notification key once left the bias promise hanging in a
  // VisionCamera app while every other canary check passed). Set back to 0 afterwards.
  func checkExposureBias(_ device: AVCaptureDevice) throws {
    let want = min(max(1.0, device.minExposureTargetBias), device.maxExposureTargetBias)
    var biasDone = false
    try withConfiguration(device) {
      device.setExposureTargetBias(want) { _ in
        self.lock.withLock { biasDone = true }
      }
    }

    check(waitFor(biasCompletionWaitS) { lock.withLock { biasDone } },
          "exposure bias completion handler within \(secondsText(biasCompletionWaitS)) s", ext: true)
    check(abs(device.exposureTargetBias - want) < biasReadbackTolerance,
          "exposure bias readback \(device.exposureTargetBias) == \(want)", ext: true)
    try withConfiguration(device) { device.setExposureTargetBias(0, completionHandler: nil) }
  }
}
