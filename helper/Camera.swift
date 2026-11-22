// SPDX-License-Identifier: Apache-2.0
// Camera.swift: the Mac camera's capture session, which delivers BGRA frames into the ring, and the choice of its
// capture format.
import AVFoundation

// The capture format the options ask for: the largest one within --size that runs at --fps.
struct FormatLimits {
  // a format runs at the wanted rate if its range reaches within this many fps of it
  static let fpsTolerance = 0.5

  let maxWidth: UInt32
  let maxHeight: UInt32
  let fps: Double

  // whether the format can run at --fps
  func supportsFPS(_ format: AVCaptureDevice.Format) -> Bool {
    format.videoSupportedFrameRateRanges.contains {
      $0.maxFrameRate >= fps - Self.fpsTolerance && $0.minFrameRate <= fps + Self.fpsTolerance
    }
  }

  func fitsSize(_ format: AVCaptureDevice.Format) -> Bool {
    let size = dimensions(format)
    return size.width <= Int32(maxWidth) && size.height <= Int32(maxHeight)
  }

  // the largest format within --size that runs at --fps, else the smallest one that runs at --fps
  func bestFormat(of device: AVCaptureDevice) -> AVCaptureDevice.Format? {
    let fitting = device.formats.filter { fitsSize($0) && supportsFPS($0) }
    if let largest = fitting.max(by: { pixelCount($0) < pixelCount($1) }) {
      return largest
    }

    return device.formats.filter(supportsFPS).min(by: { pixelCount($0) < pixelCount($1) })
  }
}

func dimensions(_ format: AVCaptureDevice.Format) -> CMVideoDimensions {
  CMVideoFormatDescriptionGetDimensions(format.formatDescription)
}

func pixelCount(_ format: AVCaptureDevice.Format) -> Int {
  Int(dimensions(format).width) * Int(dimensions(format).height)
}

// The capture session on one camera, delivering into the ring.
//
// Threading: `session` belongs to `queue`, where start and stop run in the order they were queued. The sample buffer
// delegate runs on the ring's queue, which owns `syncClock` and the log-once flags.
final class Camera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate {
  // after a session runtime error, capture restarts this much later (whole seconds, because the log prints them)
  static let runtimeErrorRestartS = 1

  let queue = DispatchQueue(label: "dev.stuntcam.camera.session", qos: .userInitiated)
  // Runs on the main queue runtimeErrorRestartS after the session reports a runtime error. Lifecycle sets it once,
  // before any session exists.
  var onRuntimeError: () -> Void = {}
  private let ring: RingWriter
  private let limits: FormatLimits
  private var session: AVCaptureSession?
  // the session's clock, for converting sample times to the host clock
  private var syncClock: CMClock?
  // each logged once per helper
  private var mismatchLogged = false
  private var formatLogged = false

  init(ring: RingWriter, limits: FormatLimits) {
    self.ring = ring
    self.limits = limits
  }

  // A new session on the device, after stopping the current one. True when it runs. Call on `queue`.
  func start(_ device: AVCaptureDevice) -> Bool {
    stop()

    // In queue order: a close queued before this start can't clear it afterwards.
    ring.queue.sync { ring.capturing = true }
    let session = AVCaptureSession()
    do {
      guard try configure(session, device) else { return false }
    } catch {
      log("camera: setup failed: \(error)")
      return false
    }

    let size = dimensions(device.activeFormat)
    let minFrameDuration = CMTimeGetSeconds(device.activeVideoMinFrameDuration)
    log("camera '\(device.localizedName)' format \(size.width)x\(size.height) minFD=\(minFrameDuration)"
        + " centerStage=\(AVCaptureDevice.isCenterStageEnabled)"
        + " reactionGestures=\(AVCaptureDevice.reactionEffectGesturesEnabled)"
        + " portrait=\(AVCaptureDevice.isPortraitEffectEnabled) studioLight=\(AVCaptureDevice.isStudioLightEnabled)")
    self.session = session
    NotificationCenter.default.addObserver(self, selector: #selector(runtimeError(_:)),
                                           name: AVCaptureSession.runtimeErrorNotification, object: session)
    startSession(session)
    return session.isRunning
  }

  // Turns Center Stage off, adds the device as input on the best format at --fps, and adds a BGRA video data output
  // that delivers on the ring's queue. Returns false, and logs why, when an input or output can't be added. The
  // configuration is then left uncommitted.
  private func configure(_ session: AVCaptureSession, _ device: AVCaptureDevice) throws -> Bool {
    AVCaptureDevice.centerStageControlMode = .app
    AVCaptureDevice.isCenterStageEnabled = false
    session.beginConfiguration()
    let input = try AVCaptureDeviceInput(device: device)
    guard session.canAddInput(input) else {
      log("camera: cannot add input")
      return false
    }

    session.addInput(input)
    if let format = limits.bestFormat(of: device) {
      try device.lockForConfiguration()
      device.activeFormat = format
      if limits.supportsFPS(format) {
        // 1/fps, counted in thousandths of a frame so that a fractional --fps keeps its precision
        device.activeVideoMinFrameDuration = CMTime(value: 1000, timescale: CMTimeScale(limits.fps * 1000))
        device.activeVideoMaxFrameDuration = device.activeVideoMinFrameDuration
      }

      device.unlockForConfiguration()
    }

    let output = AVCaptureVideoDataOutput()

    // BGRA at the format's own size
    output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
    output.alwaysDiscardsLateVideoFrames = true
    output.setSampleBufferDelegate(self, queue: ring.queue)
    guard session.canAddOutput(output) else {
      log("camera: cannot add output")
      return false
    }

    session.addOutput(output)
    session.commitConfiguration()
    return true
  }

  // The clock is read before the first sample can arrive, so that no frame is stamped with a stale or missing clock.
  private func startSession(_ session: AVCaptureSession) {
    var clock = session.synchronizationClock
    let readEarly = clock != nil
    ring.queue.sync { syncClock = clock }
    session.startRunning()
    if clock == nil {
      clock = session.synchronizationClock
      ring.queue.sync { syncClock = clock }
    }

    let clockNote = readEarly ? "read before start" : clock != nil ? "read after start" : "missing"
    log("camera session running=\(session.isRunning) sync clock \(clockNote)")
  }

  // Stops frames and tears the session down. Call on `queue`.
  func stop() {
    ring.queue.sync { ring.capturing = false }
    guard let session = session else { return }
    NotificationCenter.default.removeObserver(self, name: AVCaptureSession.runtimeErrorNotification, object: session)
    session.stopRunning()
    for output in session.outputs {
      session.removeOutput(output)
    }

    for input in session.inputs {
      session.removeInput(input)
    }

    self.session = nil
    log("camera session stopped")
  }

  @objc func runtimeError(_ notification: Notification) {
    let why = String(describing: notification.userInfo?[AVCaptureSessionErrorKey])
    log("camera: runtime error \(why); restarting in \(Self.runtimeErrorRestartS) s")
    DispatchQueue.main.asyncAfter(deadline: .now() + Double(Self.runtimeErrorRestartS)) {
      self.onRuntimeError()
    }
  }

  // One camera frame, on the ring's queue: its capture time on the host clock, a new ring object if its size differs
  // from the ring's, then its rows copied into a slot.
  func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                     from connection: AVCaptureConnection) {
    guard ring.capturing, let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
    let arrivalNs = sch_now_ns()
    let captureNs = hostClockNs(sampleBuffer)
    guard CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_32BGRA else {
      if !formatLogged {
        formatLogged = true
        log("camera: frame format is not BGRA; dropped")
      }

      return
    }

    let width = UInt32(CVPixelBufferGetWidth(pixelBuffer))
    let height = UInt32(CVPixelBufferGetHeight(pixelBuffer))
    if width != ring.width || height != ring.height {
      guard replaceRing(width: width, height: height) else { return }
    }

    ring.countCameraFrame(delayNs: Int64(arrivalNs) - Int64(captureNs))
    CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }
    guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
    let sourceBytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let rowBytes = Int(width) * Int(RingWriter.bytesPerPixel)
    ring.publish(captureNs: captureNs, flags: 0) { pixels in
      sch_copy_rows(pixels, rowBytes, base.assumingMemoryBound(to: UInt8.self), sourceBytesPerRow, rowBytes, height)
    }
  }

  // the sample's presentation time on the host clock, in ns (never negative)
  private func hostClockNs(_ sampleBuffer: CMSampleBuffer) -> UInt64 {
    var time = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
    if let clock = syncClock {
      time = CMSyncConvertTime(time, from: clock, to: CMClockGetHostTimeClock())
    }

    let nanoseconds = CMTimeConvertScale(time, timescale: 1_000_000_000, method: .roundHalfAwayFromZero).value
    return UInt64(max(0, nanoseconds))
  }

  // The camera delivers another size than the ring has: a new object with that geometry.
  private func replaceRing(width: UInt32, height: UInt32) -> Bool {
    if !mismatchLogged {
      mismatchLogged = true
      log("camera frames are \(width)x\(height), ring is \(ring.width)x\(ring.height): new ring")
    }

    return ring.create(width: width, height: height)
  }
}
