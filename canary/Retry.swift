// SPDX-License-Identifier: Apache-2.0
// The end-of-run retry of a photo stage whose only miss was the frame gap.
//
// A photo stage (jpeg, 4x3 or hvc1) whose only miss is the frame-gap check is run once more, after every other stage,
// in a session of its own and with the same threshold.
//
// Why: right after a boot the frame pump can stall. On 27.0 an HEVC photo stalled it for 129-158 ms on 3 of 37 first
// launches, and for 116-886 ms when the canary started about 2 s after bootstatus.
//
// The limits: a retry in the same, already warm process can't tell that stall from a regression that only hits a
// process's first photo (for example, lazy encoder setup on the pump). So only the gap check is retried, the CLI
// reports a retried pass as such and caches it only briefly, and on 27.x the CLI waits out the post-boot window before
// it launches the canary (the settle floor).
//
// The outcome: a retry that misses again leaves the run INCONCLUSIVE, and a content FAIL in the retry is a FAIL. The
// RESULT line lists each first attempt in retried:[… -> retry ok|SLOW|FAIL]. Nothing else is retried in-process: a slow
// first frame, a low frame count, or a late or undelivered photo stays INCONCLUSIVE.
import AVFoundation

extension Canary {
  // Runs a stage's body. If its only misses were frame-gap misses, they are taken back out of the verdict and retry is
  // queued for runRetries.
  func attempt(_ name: String, retry: @escaping () -> Void, _ body: () -> Void) {
    let before = verdict.coreCounts()
    body()
    let (misses, gapOnly) = verdict.withdrawRetryableMisses(since: before, where: isFrameGapMiss)
    if gapOnly {
      log("RETRY-LATER \(name): \(misses.joined(separator: "; "))")
      retryLater.append((name, misses, retry))
    }
  }

  // runs each queued retry and records its outcome for the RESULT line's retried:[…]
  func runRetries() {
    for retry in retryLater {
      let before = verdict.coreCounts()
      log("RETRY \(retry.stage)")
      retry.run()
      let outcome = verdict.recordRetry(firstMisses: retry.first, since: before)
      log("RETRY \(retry.stage) -> \(outcome)")
    }
  }

  // Runs a retried photo in a session of its own: the back camera on `format`, a VideoDataOutput for the gap check's
  // frames, and a photo output.
  // The photo uses the first attempt's codec and photo dimensions. Without dimensions it uses the format's largest.
  func retryPhoto(_ device: AVCaptureDevice, _ format: AVCaptureDevice.Format?, codec: AVVideoCodecType, label: String,
                  photoDimensions: CMVideoDimensions? = nil) {
    let session = AVCaptureSession()
    session.beginConfiguration()
    session.sessionPreset = .inputPriority
    verdict.stage = label
    guard let input = try? AVCaptureDeviceInput(device: device), session.canAddInput(input) else {
      session.commitConfiguration()
      check(false, "retry session input")
      return
    }

    session.addInput(input)
    let videoOutput = AVCaptureVideoDataOutput()
    videoOutput.alwaysDiscardsLateVideoFrames = true
    videoOutput.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.retry"))
    session.addOutput(videoOutput)
    let photoOutput = AVCapturePhotoOutput()
    session.addOutput(photoOutput)
    if let format = format {
      do {
        try withConfiguration(device) { device.activeFormat = format }
      } catch { check(false, "retry lock \(error)") }
    }

    session.commitConfiguration()
    session.startRunning()
    firstFrame("\(label) retry")
    pause(formatSettleS)
    if let size = photoDimensions {
      photoOutput.maxPhotoDimensions = size
    } else if let largest = largestPhotoDimensions(device.activeFormat) {
      photoOutput.maxPhotoDimensions = largest
    }

    capture(photoOutput, codec: codec, label: label)
    session.stopRunning()
    for output in session.outputs { session.removeOutput(output) }
    for input in session.inputs { session.removeInput(input) }
    pause(sessionReleaseS)
  }
}
