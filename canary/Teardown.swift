// SPDX-License-Identifier: Apache-2.0
// Teardown of the first session, then more sessions on the same camera: lifetime and dangling-pointer bugs surface
// here.
import UIKit
import AVFoundation

// the pause after the first session is torn down
let teardownSettleS = 0.3
// the frame windows of the second sessions: the first of them (S2) gets a longer one
let firstSecondSessionWindow = FrameWindow(seconds: 0.7, minFrames: 8)
let laterSecondSessionWindow = FrameWindow(seconds: 0.4, minFrames: 4)

extension Canary {
  func teardown(_ main: MainSession) {
    let session = main.session
    verdict.stage = "teardown"
    session.stopRunning()
    for output in session.outputs { session.removeOutput(output) }
    for input in session.inputs { session.removeInput(input) }
    onMain {
      preview?.session = nil
      preview?.removeFromSuperlayer()
      preview = nil
    }

    controlObservers = []
    pause(teardownSettleS)
  }

  // Sessions 2, 3 and 4 in a row, each released before the next, so one can be allocated at a freed one's address
  // (an earlier bug: it inherited the dead session's sinks). The first session is still alive, so session 2 alone
  // could never land on a freed session's address.
  func secondSessions(_ device: AVCaptureDevice) {
    verdict.stage = "second-session"
    for number in 2...4 {
      autoreleasepool {
        let session = AVCaptureSession()
        session.beginConfiguration()
        if let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) {
          session.addInput(input)
        } else {
          check(false, "session \(number) input")
        }

        let videoOutput = AVCaptureVideoDataOutput()
        videoOutput.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.vdo\(number)"))
        session.addOutput(videoOutput)
        session.commitConfiguration()
        session.startRunning()
        firstFrame("S\(number)")
        let window = number == 2 ? firstSecondSessionWindow : laterSecondSessionWindow
        checkFrameWindow("S\(number)", window)
        session.stopRunning()
        for output in session.outputs { session.removeOutput(output) }
        for input in session.inputs { session.removeInput(input) }
      }

      pause(sessionReleaseS)
    }
  }
}
