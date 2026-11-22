// SPDX-License-Identifier: Apache-2.0
// The Canary class: the state AVFoundation's callbacks (Delegates.swift) fill in, and run(), the stages in order. Each
// stage is an extension in a file of its own.
//
// The core checks:
// - discovery, and the virtual devices of the 8-camera profile;
// - frames, through a VideoDataOutput and the preview layer;
// - QR, through a MetadataOutput whose availableMetadataObjectTypes contains qr;
// - photos in JPEG, at 4:3 and in HEVC, with their callback order and didFinishCaptureFor;
// - reconfiguring while running: an activeFormat change, then swapping outputs;
// - a movie, a capture followed at once by stopRunning, and background and foreground;
// - teardown followed by more sessions, and the front camera with mirroring.
// The extended checks always run and are reported separately: the engine's operation trace, focus and exposure
// metering, the torch, zoom readback, exposure bias completion and readback, and audio (a microphone input with an
// AVCaptureAudioDataOutput).
import UIKit
import AVFoundation

// the pause between the last of the second sessions and the front camera's session
let beforeFrontS = 0.2

final class Canary: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, AVCaptureMetadataOutputObjectsDelegate,
  AVCapturePhotoCaptureDelegate, AVCaptureFileOutputRecordingDelegate, AVCaptureAudioDataOutputSampleBufferDelegate {
  // guards the state below that the callbacks write
  let lock = NSLock()
  // objects a stage creates that must stay allocated until the process exits, though no later stage names them: freed
  // at teardown, their addresses could be reused by the second sessions' objects, which that stage is there to catch
  var keptUntilExit: [AnyObject] = []
  var window: UIWindow!
  var preview: AVCaptureVideoPreviewLayer?

  // frames from every VideoDataOutput, since the last resetFrames()
  var frames: [FrameRecord] = []
  var dropped = 0
  // metadata objects (QR codes) seen
  var qrObjects: [(type: AVMetadataObject.ObjectType, text: String?, bounds: CGRect)] = []
  // session notifications and movie events, by name ("didStart", "didStop", "runtimeError …", "movieStarted", …)
  var events: [String] = []
  var backgroundSeen = false
  var foregroundSeen = false

  // The photo in flight: its delegate callbacks in order, the photos delivered, and whether didFinishCaptureFor came.
  var photoEvents: [String] = []
  var photos: [(data: Data?, error: Error?, timestamp: Double, metadata: [String: Any])] = []
  var photoDone = false
  // One entry per capture() for the trace stage: the settings uniqueID, the delivered timestamp and the EXIF capture
  // time.
  // A capture is kept only if it had no FAIL and no timing miss other than the frame gap. A failed photo's trace ops
  // may be missing.
  // The EXIF time is in seconds since 1970. It is nil when the photo lacks SubsecTimeOriginal or OffsetTimeOriginal,
  // the two tags the engine writes.
  var tracePhotos: [(settingsID: Int64, timestamp: Double, exif: Double?)] = []
  // For the photo-then-stop stage: how many didStopRunning notifications had arrived when the last didFinishCaptureFor
  // came, and the time it came.
  var stopsAtFinish = -1
  var finishAt = Date.distantPast

  var movieDone: (URL, Error?)? = nil
  // key-value observers of the device controls, kept until teardown
  var controlObservers: [NSKeyValueObservation] = []
  // front-mirror: the next 32-bit BGRA frame is reduced to a brightness grid when gridRequested is set
  var gridRequested = false
  var grid: [Float]? = nil
  // audio buffers delivered, their timestamps and the sample rate of the last one
  var audioBufferCount = 0
  var audioPTS: [Double] = []
  var audioRate = 0.0
  // photo stages to run once more after every other stage (Retry.swift)
  var retryLater: [(stage: String, first: [String], run: () -> Void)] = []

  func recordEvent(_ name: String) {
    lock.withLock { events.append(name) }
  }

  // the number of recorded events whose name starts with `name`
  func eventCount(_ name: String) -> Int {
    lock.withLock { events.filter { $0.hasPrefix(name) }.count }
  }

  func snapshotFrames() -> [FrameRecord] {
    lock.withLock { frames }
  }

  func resetFrames() {
    lock.withLock {
      frames = []
      dropped = 0
    }
  }

  // ---- the run: one function per stage, in this order (each sets its stage name for the check lines)
  func run() {
    observeNotifications()
    let device = discoverDevices()
    checkVirtualDevices()

    // the first session: VideoDataOutput, metadata and photo outputs, preview
    let main = configureSession(device)
    startSession(main)
    let firstSize = checkFirstFrames(device)   // frames through AVFoundation's own consumer
    checkQR()
    capturePhotos(device, main)
    traceCheck()
    checkDeviceControls(device)   // the extended (ext=) device-control checks

    // reconfigure while running: change activeFormat, then swap outputs
    reconfigureFormat(device, firstSize, main.format1080p)
    let movie = reconfigureOutputs(main)
    recordMovie(device, movie)
    photoThenStop(main)
    backgroundForeground(main, movie)

    // teardown, more sessions on the same camera, the front camera, audio
    teardown(main)
    secondSessions(device)
    pause(beforeFrontS)
    frontMirror()
    audio()

    let lastStage = verdict.stage
    runRetries()
    verdict.stage = lastStage   // the overall check keeps its stage name
    check(eventCount("runtimeError") == 0, "no AVCaptureSessionRuntimeError overall (\(eventCount("runtimeError")))")
    finish()
  }

  func observeNotifications() {
    let center = NotificationCenter.default
    center.addObserver(forName: AVCaptureSession.runtimeErrorNotification, object: nil, queue: nil) { notification in
      self.recordEvent("runtimeError \(notification.userInfo?[AVCaptureSessionErrorKey] ?? "")")
      log("runtimeError \(notification.userInfo?[AVCaptureSessionErrorKey] ?? "")")
    }

    center.addObserver(forName: AVCaptureSession.didStartRunningNotification, object: nil, queue: nil) { _ in
      self.recordEvent("didStart")
    }

    center.addObserver(forName: AVCaptureSession.didStopRunningNotification, object: nil, queue: nil) { _ in
      self.recordEvent("didStop")
    }

    center.addObserver(forName: AVCaptureSession.wasInterruptedNotification, object: nil, queue: nil) { _ in
      self.recordEvent("interrupted")
    }

    center.addObserver(forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: nil) { _ in
      self.backgroundSeen = true
      log("app did enter background")
    }

    center.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: nil) { _ in
      self.foregroundSeen = true
      log("app will enter foreground")
    }
  }
}
