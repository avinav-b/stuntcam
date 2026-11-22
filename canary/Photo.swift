// SPDX-License-Identifier: Apache-2.0
// Photo stages: one capture and its checks (capture), the three photo stages on the first session (capturePhotos), and
// a capture followed at once by stopRunning (photoThenStop).
import AVFoundation
import ImageIO

// the longest gap between frames allowed while a photo is taken (timing)
let maxPhotoFrameGapMs = 100
// the smallest width of the 4:3 photo size the photo-4x3 stage looks for
let min4x3PhotoWidth: Int32 = 4000
// photo-then-stop: how long to wait for the photo before it counts as never completing, and how soon after its
// didFinishCaptureFor stopRunning must return (timing)
let photoThenStopNeverS = 15.0
let stopReturnMaxMs = 250

// The photo's timestamp may lie up to photoTimestampEarlyS before the capturePhoto call and up to photoTimestampLateS
// after its completion.
let photoTimestampEarlyS = 0.5
let photoTimestampLateS = 0.05
// the pause after activeFormat changes for the 4:3 photo (and before a retried photo), so frames flow again
let formatSettleS = 0.3

// capture()'s deadlines: the photo is delivered within photoDeliverS and completes photoCompleteS after that; a late
// one is drained for photoLateDrainS more before it counts as never completing
let photoDeliverS = 4.0
let photoCompleteS = 1.5
let photoLateDrainS = 8.0
// the start of the frame-gap check's message, which marks its misses (Retry.swift retries a stage that missed only
// this check)
let frameGapCheck = "longest frame gap during the photo"
func isFrameGapMiss(_ entry: String) -> Bool { entry.contains(frameGapCheck) }

extension Canary {
  func resetPhotoState() {
    lock.withLock {
      photoEvents = []
      photos = []
      photoDone = false
    }
  }

  // Takes one photo with codec at the output's maxPhotoDimensions. The stage label defaults to photo-<codec>.
  // It checks that the photo is delivered and completes in time, that no long frame gap happens meanwhile, and that
  // the callbacks come in order. photoChecks then checks the photo itself.
  // A clean capture is kept for the trace stage.
  func capture(_ out: AVCapturePhotoOutput, codec: AVVideoCodecType, label: String? = nil) {
    verdict.stage = label ?? "photo-\(codec.rawValue)"
    let before = verdict.coreCounts()
    resetPhotoState()
    let settings = AVCapturePhotoSettings(format: [AVVideoCodecKey: codec])
    settings.maxPhotoDimensions = out.maxPhotoDimensions
    let requestedAt = hostNow()
    resetFrames()
    out.capturePhoto(with: settings, delegate: self)
    let arrived = waitFor(photoDeliverS) { lock.withLock { !photos.isEmpty || photoDone } }
    guard waitForPhotoCompletion() else { return }
    let completedAt = hostNow()
    if !arrived { check(false, timing: true, "photo delivered within \(secondsText(photoDeliverS)) s") }
    checkFrameGap(from: requestedAt, to: completedAt)
    guard let shot = photoChecks(out, codec: codec, requestedAt: requestedAt, completedAt: completedAt) else { return }
    let clean = verdict.hasOnlyMisses(since: before, matching: isFrameGapMiss)
    if clean { tracePhotos.append((settings.uniqueID, shot.timestamp, exifTime(shot.exif))) }
  }

  // Waits for didFinishCaptureFor, and returns false if it never comes.
  // A late one is still waited for, so that its callbacks can't land in the next stage. That happened on a starved
  // hosted runner.
  // Late is a timing miss, and the content checks still run. Never is a FAIL.
  func waitForPhotoCompletion() -> Bool {
    let completeS = photoDeliverS + photoCompleteS
    let neverS = completeS + photoLateDrainS
    if waitFor(photoCompleteS, { lock.withLock { photoDone } }) { return true }
    let lateSince = Date()
    let late = waitFor(photoLateDrainS) { lock.withLock { photoDone } }
    let how = late
      ? String(format: "completed %.1f s later", Date().timeIntervalSince(lateSince))
      : "not after \(secondsText(neverS)) s"
    check(false, timing: true, "photo completed within \(secondsText(completeS)) s (" + how + ")")
    if !late { check(false, "photo never completed (\(secondsText(neverS)) s)") }
    return late
  }

  // Encoding a photo on the frame pump stalls preview and video data output frames for 125-283 ms per photo (2 s in a
  // burst). On its own queue the longest gap is about 35 ms. A gap over 100 ms (3 frame intervals at 30 fps) means the
  // pump is stalled again.
  func checkFrameGap(from start: Double, to end: Double) {
    let arrivals = snapshotFrames().map { $0.arrival }.filter { $0 >= start && $0 <= end }
    var gap = 0.0
    var previous = start
    for arrival in arrivals {
      gap = max(gap, arrival - previous)
      previous = arrival
    }

    gap = max(gap, end - previous)
    let gapMs = Int(gap * millisecondsPerSecond)
    check(gap <= Double(maxPhotoFrameGapMs) / millisecondsPerSecond, timing: true,
          "\(frameGapCheck) \(gapMs) ms (<= \(maxPhotoFrameGapMs) ms, \(arrivals.count) frames)")
  }

  // The delivered photo, requested at host time requestedAt and completed at completedAt: callback order, no error, a
  // timestamp on the host clock, and a file that decodes as the codec's container at maxPhotoDimensions with EXIF.
  // Returns the photo's timestamp and EXIF dictionary, or nil when there is no photo to check further.
  func photoChecks(_ out: AVCapturePhotoOutput, codec: AVVideoCodecType, requestedAt: Double,
                   completedAt: Double) -> (timestamp: Double, exif: [String: Any]?)? {
    let (callbacks, delivered) = lock.withLock { (photoEvents, photos) }
    let order = callbacks.map { $0.split(separator: " ").first.map(String.init) ?? $0 }
    check(order.starts(with: ["willBegin", "willCapture", "didCapture", "processed"]), "callback order \(order)")
    guard let photo = delivered.first else {
      check(false, "AVCapturePhoto delivered before didFinishCaptureFor (events \(callbacks))")
      return nil
    }

    check(photo.error == nil, "photo error \(String(describing: photo.error))")
    let window = "[\(String(format: "%.3f", requestedAt)), \(String(format: "%.3f", completedAt))]"
    check(photo.timestamp >= requestedAt - photoTimestampEarlyS && photo.timestamp <= completedAt + photoTimestampLateS,
          "photo.timestamp on host clock (\(String(format: "%.3f", photo.timestamp)) in \(window))")
    guard let data = photo.data, let source = CGImageSourceCreateWithData(data as CFData, nil),
          let props = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [String: Any] else {
      check(false, "fileDataRepresentation decodes (bytes=\(photo.data?.count ?? -1))")
      return nil
    }

    let type = CGImageSourceGetType(source) as String? ?? "?"
    let width = props[kCGImagePropertyPixelWidth as String] as? Int ?? 0
    let height = props[kCGImagePropertyPixelHeight as String] as? Int ?? 0
    let want = codec == .jpeg ? "public.jpeg" : "public.heic"
    check(type == want, "container \(type) == \(want)")
    let maxDimensions = out.maxPhotoDimensions
    check(max(width, height) == Int(max(maxDimensions.width, maxDimensions.height))
            && min(width, height) == Int(min(maxDimensions.width, maxDimensions.height)),
          "pixels \(width)x\(height) == maxPhotoDimensions \(maxDimensions.width)x\(maxDimensions.height)")
    let exif = props[kCGImagePropertyExifDictionary as String] as? [String: Any]
    let hasOrientation = props[kCGImagePropertyOrientation as String] != nil
    check(hasOrientation && exif?[kCGImagePropertyExifDateTimeOriginal as String] != nil,
          "EXIF Orientation + DateTimeOriginal present")
    check(photo.metadata[kCGImagePropertyOrientation as String] != nil, "AVCapturePhoto.metadata has Orientation")
    return (photo.timestamp, exif)
  }

  // EXIF DateTimeOriginal + SubsecTimeOriginal + OffsetTimeOriginal as seconds since 1970 (nil if one is missing)
  func exifTime(_ exif: [String: Any]?) -> Double? {
    guard let exifDictionary = exif,
          let dateTime = exifDictionary[kCGImagePropertyExifDateTimeOriginal as String] as? String,
          let subseconds = exifDictionary[kCGImagePropertyExifSubsecTimeOriginal as String] as? String,
          let offset = exifDictionary[kCGImagePropertyExifOffsetTimeOriginal as String] as? String else { return nil }
    let formatter = DateFormatter()
    formatter.locale = Locale(identifier: "en_US_POSIX")
    formatter.dateFormat = "yyyy:MM:dd HH:mm:ssxxx"
    guard let date = formatter.date(from: dateTime + offset), let fraction = Double("0." + subseconds) else {
      return nil
    }

    return date.timeIntervalSince1970 + fraction
  }

  // JPEG on the 16:9 format, JPEG on a 4:3 format at its full size, then HEVC; each can be retried at the end
  func capturePhotos(_ device: AVCaptureDevice, _ main: MainSession) {
    let photo = main.photo
    let format1080p = main.format1080p
    attempt("photo-jpeg",
            retry: { [self] in retryPhoto(device, format1080p, codec: .jpeg, label: "photo-jpeg") }) {
      capture(photo, codec: .jpeg)
    }

    capture4x3Photo(device, main)
    if photo.availablePhotoCodecTypes.contains(.hevc) {
      attempt("photo-hvc1",
              retry: { [self] in retryPhoto(device, format1080p, codec: .hevc, label: "photo-hvc1") }) {
        capture(photo, codec: .hevc)
      }
    } else {
      verdict.stage = "photo-hvc1"
      check(false, "HEVC available (\(photo.availablePhotoCodecTypes))")
    }
  }

  // A 4:3 format at its full size: on the 16:9 format the engine's fallback size equals the max, so a missing
  // photo-size fix went unnoticed in a mutation run. Switches back to the 16:9 format afterwards.
  func capture4x3Photo(_ device: AVCaptureDevice, _ main: MainSession) {
    let photo = main.photo
    let is4x3 = { (size: CMVideoDimensions) in size.width * 3 == size.height * 4 }
    let format4x3 = device.formats.first { format in
      format.supportedMaxPhotoDimensions.contains { is4x3($0) && $0.width >= min4x3PhotoWidth }
    }

    guard let format4x3,
          let photoSize = format4x3.supportedMaxPhotoDimensions.filter(is4x3).max(by: { $0.width < $1.width }) else {
      verdict.stage = "photo-4x3"
      check(false, "a format with a 4:3 photo size >= \(min4x3PhotoWidth) wide")
      return
    }

    // read before the switch: the 4:3 format resets it (restoring that throws)
    let previousMaxDimensions = photo.maxPhotoDimensions
    do {
      try withConfiguration(device) { device.activeFormat = format4x3 }
    } catch { check(false, "lock \(error)") }
    pause(formatSettleS)
    photo.maxPhotoDimensions = photoSize
    attempt("photo-4x3",
            retry: { [self] in
              retryPhoto(device, format4x3, codec: .jpeg, label: "photo-4x3", photoDimensions: photoSize)
            }) {
      capture(photo, codec: .jpeg, label: "photo-4x3")
    }

    try? withConfiguration(device) { if let format = main.format1080p { device.activeFormat = format } }

    // the 16:9 format does not support 4:3 sizes; the HEVC capture would throw
    photo.maxPhotoDimensions = previousMaxDimensions
    pause(formatSettleS)
  }

  // A capture followed at once by stopRunning, as when a user takes a photo and leaves the screen.
  // The photo and both completions must arrive.
  // didFinishCaptureFor must come before the session's didStopRunning notification, because stopRunning waits for
  // photos in flight. Without that wait AVFoundation still delivers the photo, but after the stop.
  func photoThenStop(_ main: MainSession) {
    let session = main.session
    verdict.stage = "photo-then-stop"
    let stopsBefore = eventCount("didStop")
    resetPhotoState()

    let settings = AVCapturePhotoSettings(format: [AVVideoCodecKey: AVVideoCodecType.jpeg])
    main.photo.capturePhoto(with: settings, delegate: self)
    let stopCalledAt = Date()
    session.stopRunning()   // at once, so the encode is still in flight
    let stopReturnedAt = Date()

    let stopDone = waitFor(photoThenStopNeverS) { lock.withLock { photoDone } }
    let (stopEvents, stopPhotos) = lock.withLock { (photoEvents, photos) }
    check(stopDone, "didFinishCaptureFor after capture + stopRunning (events \(stopEvents))")
    let firstError = String(describing: stopPhotos.first?.error)
    check(stopPhotos.count == 1 && stopPhotos[0].error == nil && stopPhotos[0].data != nil,
          "photo delivered without error after stopRunning (n=\(stopPhotos.count) err=\(firstError))")
    check(waitFor(sessionNotificationWaitS) { eventCount("didStop") > stopsBefore },
          "didStopRunning notification after capture + stopRunning")

    let stopsAtCompletion = lock.withLock { stopsAtFinish }
    let finishedFirst = stopDone && stopsAtCompletion == stopsBefore
    let stopsMeanwhile = stopsAtCompletion - stopsBefore
    check(finishedFirst,
          "didFinishCaptureFor before didStopRunning (stop notifications at completion: \(stopsMeanwhile))")

    // The engine reports the stop once the photo's didFinishCaptureFor has run, and waits at most 500 ms for it.
    // A stop that returns about 500 ms after that callback means the wait ran out: the done-mark never came, and the
    // order check above passed only because of the bound. So the threshold is half the bound.
    // The time runs from the callback, not from the call. The call also includes the encode (100-350 ms) and any late
    // callback (the 300 ms positive control).
    let finishedAt = lock.withLock { finishAt }
    if finishedFirst {
      let gap = stopReturnedAt.timeIntervalSince(finishedAt)
      check(gap < Double(stopReturnMaxMs) / millisecondsPerSecond, timing: true,
            "stopRunning returned \(Int(gap * millisecondsPerSecond)) ms after didFinishCaptureFor"
              + " (< \(stopReturnMaxMs) ms;"
              + " the call took \(Int(stopReturnedAt.timeIntervalSince(stopCalledAt) * millisecondsPerSecond)) ms)")
    }

    let startsBefore = eventCount("didStart")

    // no frame check here: no VideoDataOutput is attached at this point (the bgfg stage adds one)
    session.startRunning()
    check(waitFor(sessionNotificationWaitS) { eventCount("didStart") > startsBefore },
          "didStartRunning notification after the restart")
  }
}
