// SPDX-License-Identifier: Apache-2.0
// The stages on the first session: configure it, start it, its first frames and preview, QR, reconfigure it while
// running, and background / foreground.
import UIKit
import AVFoundation

// how long the metadata output may take to list the QR type, and the QR code to arrive
let metadataTypesWaitS = 1.0
let qrWaitS = 2.5
// how long the preview layer may take to show a frame
let previewWaitS = 1.0
// the frame windows: the first session at its first format (A), after the format change (B), and after the foreground
let firstWindow = FrameWindow(seconds: 1.0, minFrames: 12)
let reconfiguredWindow = FrameWindow(seconds: 0.8, minFrames: 10)
let foregroundWindow = FrameWindow(seconds: 0.8, minFrames: 10)
// the pause after activeFormat changes while running, before the first-frame check
let formatChangeSettleS = 0.4
// background / foreground: how long to wait for the runner to background the app and bring it back, the pause after
// the foreground, and how long to wait for the didStopRunning notification
let backgroundWaitS = 5.0
let foregroundWaitS = 5.0
let afterForegroundS = 0.3
let stopNotificationWaitS = 1.0

extension Canary {
  // the first session (VideoDataOutput + metadata + photo + preview), built by configureSession and used by later
  // stages
  struct MainSession {
    let session: AVCaptureSession
    let videoOutput: AVCaptureVideoDataOutput
    let metadataOutput: AVCaptureMetadataOutput
    let photo: AVCapturePhotoOutput
    let format1080p: AVCaptureDevice.Format?   // the 16:9 1920x1080 format, if present: the stages switch back to it
  }

  // the back camera with a VideoDataOutput, a metadata output and a photo output, on the 1080p format at its
  // largest photo size, shown in a preview layer; the run ends here if the input can't be added
  func configureSession(_ device: AVCaptureDevice) -> MainSession {
    verdict.stage = "configure"
    let session = AVCaptureSession()
    session.beginConfiguration()
    session.sessionPreset = .inputPriority
    guard let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) else {
      check(false, "add input")
      finish()
    }

    keptUntilExit.append(input)
    session.addInput(input)
    let videoOutput = AVCaptureVideoDataOutput()
    videoOutput.alwaysDiscardsLateVideoFrames = true
    videoOutput.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.vdo"))
    check(session.canAddOutput(videoOutput), "canAddOutput(VideoData)")
    session.addOutput(videoOutput)
    let metadataOutput = AVCaptureMetadataOutput()
    check(session.canAddOutput(metadataOutput), "canAddOutput(Metadata)")
    session.addOutput(metadataOutput)
    metadataOutput.setMetadataObjectsDelegate(self, queue: DispatchQueue(label: "canary.meta"))
    let photo = AVCapturePhotoOutput()
    check(session.canAddOutput(photo), "canAddOutput(Photo)")
    session.addOutput(photo)
    let format1080p = device.formats.first { is1080p($0) && reaches30FPS($0) }
    do {
      try withConfiguration(device) { if let format = format1080p { device.activeFormat = format } }
    } catch { check(false, "lockForConfiguration \(error)") }
    if let largest = largestPhotoDimensions(device.activeFormat) {
      photo.maxPhotoDimensions = largest
    }

    session.commitConfiguration()
    addPreview(session)
    return MainSession(session: session, videoOutput: videoOutput, metadataOutput: metadataOutput, photo: photo,
                       format1080p: format1080p)
  }

  func addPreview(_ session: AVCaptureSession) {
    onMain {
      let previewLayer = AVCaptureVideoPreviewLayer(session: session)
      previewLayer.frame = window.bounds
      previewLayer.videoGravity = .resizeAspectFill
      window.rootViewController!.view.layer.addSublayer(previewLayer)
      preview = previewLayer
    }
  }

  func startSession(_ main: MainSession) {
    let session = main.session
    let metadataOutput = main.metadataOutput
    verdict.stage = "start"
    session.startRunning()
    check(waitFor(sessionNotificationWaitS) { eventCount("didStart") >= 1 }, "didStartRunning notification")
    check(session.isRunning, "isRunning")

    // like expo-camera: only enable the types the output says are available
    check(waitFor(metadataTypesWaitS) { metadataOutput.availableMetadataObjectTypes.contains(.qr) },
          "availableMetadataObjectTypes contains qr"
            + " (\(metadataOutput.availableMetadataObjectTypes.map { $0.rawValue }))")
    if metadataOutput.availableMetadataObjectTypes.contains(.qr) {
      metadataOutput.metadataObjectTypes = [.qr]
    }
  }

  // returns the format's dimensions the frames were checked against
  func checkFirstFrames(_ device: AVCaptureDevice) -> CMVideoDimensions {
    verdict.stage = "frames"
    let firstSize = dimensions(device.activeFormat)
    firstFrame("A")   // the cold start time: never retried in-process
    checkFrameWindow("A", firstWindow, expectedWidth: Int(firstSize.width), expectedHeight: Int(firstSize.height))
    verdict.stage = "preview"
    check(waitFor(previewWaitS) { previewHasContents() }, "preview layer (or a sublayer) has contents")
    return firstSize
  }

  // a QR code with an expected payload (Config.swift) arrives as a metadata object
  func checkQR() {
    verdict.stage = "qr"
    let gotQR = waitFor(qrWaitS) {
      lock.withLock { qrObjects.contains { $0.type == .qr && config.expectedQRs.contains($0.text ?? "") } }
    }

    let seen = lock.withLock { qrObjects.prefix(3).map { "\($0.type.rawValue):\($0.text ?? "nil"):\($0.bounds)" } }
    check(gotQR, "QR '\(config.expectedQR)' via AVMetadataMachineReadableCodeObject (seen \(seen))")
  }

  // change activeFormat while running: frames continue at the new size; then back to the 1080p format
  func reconfigureFormat(_ device: AVCaptureDevice, _ firstSize: CMVideoDimensions,
                         _ format1080p: AVCaptureDevice.Format?) {
    verdict.stage = "reconfigure-format"
    let other = device.formats.first {
      let size = dimensions($0)
      return size.width != firstSize.width || size.height != firstSize.height
    }

    guard let format = other else {
      check(false, "no second format to switch to")
      return
    }

    do {
      try withConfiguration(device) { device.activeFormat = format }
    } catch { check(false, "lock \(error)") }
    pause(formatChangeSettleS)
    firstFrame("B")
    let newSize = dimensions(format)
    checkFrameWindow("B", reconfiguredWindow, expectedWidth: Int(newSize.width), expectedHeight: Int(newSize.height))
    try? withConfiguration(device) { if let format = format1080p { device.activeFormat = format } }
  }

  // swap the VideoDataOutput and metadata output for a movie file output while running
  func reconfigureOutputs(_ main: MainSession) -> AVCaptureMovieFileOutput {
    let session = main.session
    verdict.stage = "reconfigure-outputs"
    let movie = AVCaptureMovieFileOutput()
    session.beginConfiguration()
    session.removeOutput(main.videoOutput)
    session.removeOutput(main.metadataOutput)
    check(session.canAddOutput(movie), "canAddOutput(MovieFile) after removing VDO")
    session.addOutput(movie)
    session.commitConfiguration()
    check(session.isRunning && eventCount("runtimeError") == 0, "still running, no runtime error after reconfigure")
    check(movie.connection(with: .video)?.isActive == true, "movie connection active")
    return movie
  }

  // background / foreground: the runner backgrounds the app when it sees the BG-REQUEST log line; without that,
  // stop/start stands in for it
  func backgroundForeground(_ main: MainSession, _ movie: AVCaptureMovieFileOutput) {
    let session = main.session
    verdict.stage = "bgfg"
    let stopsBefore = eventCount("didStop")
    session.beginConfiguration()
    session.removeOutput(movie)
    let videoOutput = AVCaptureVideoDataOutput()
    keptUntilExit.append(videoOutput)
    videoOutput.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.vdo2"))
    session.addOutput(videoOutput)
    session.commitConfiguration()
    log("BG-REQUEST")
    if waitFor(backgroundWaitS, { backgroundSeen }) {
      session.stopRunning()   // what apps do in sceneDidEnterBackground
      _ = waitFor(foregroundWaitS) { foregroundSeen }  // the check below reports a timeout
      check(foregroundSeen, "foreground notification after background")
      pause(afterForegroundS)
    } else {
      log("no real background within \(secondsText(backgroundWaitS)) s -> simulated (stop/start)")
      session.stopRunning()
    }

    check(waitFor(stopNotificationWaitS) { eventCount("didStop") > stopsBefore }, "didStopRunning notification")
    session.startRunning()
    firstFrame("after-fg")
    checkFrameWindow("after-fg", foregroundWindow)
    check(waitFor(previewWaitS) { previewHasContents() }, "preview has contents after foreground")
  }
}
