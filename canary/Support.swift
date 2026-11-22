// SPDX-License-Identifier: Apache-2.0
// Small helpers the stages share.
import AVFoundation

// Time units, for converting between AVFoundation's seconds and the ms and ns the checks print and the engine logs.
let nanosecondsPerSecond = 1e9
let nanosecondsPerMillisecond = 1e6
let microsecondsPerSecond = 1e6
let millisecondsPerSecond = 1000.0

// now on the host clock, in seconds: the clock AVFoundation stamps frames and photos with
func hostNow() -> Double { CMClockGetTime(CMClockGetHostTimeClock()).seconds }

// Sleeps the calling thread (the canary's own) for the given time.
func pause(_ seconds: Double) {
  usleep(useconds_t((seconds * microsecondsPerSecond).rounded()))
}

// How long to wait for a session's didStartRunning or didStopRunning notification.
let sessionNotificationWaitS = 2.0
// The pause after a session is torn down, before the next one is made.
let sessionReleaseS = 0.15

// how often waitFor polls its condition
let waitPollMicroseconds: useconds_t = 20_000
// Polls `condition` until it holds or `seconds` have passed, and returns its last value.
func waitFor(_ seconds: Double, _ condition: () -> Bool) -> Bool {
  let deadline = Date().addingTimeInterval(seconds)
  while Date() < deadline {
    if condition() { return true }
    usleep(waitPollMicroseconds)
  }
  return condition()
}

// Runs body between lockForConfiguration and unlockForConfiguration.
// If lockForConfiguration throws, body does not run and the error is passed on.
func withConfiguration(_ device: AVCaptureDevice, _ body: () -> Void) throws {
  try device.lockForConfiguration()
  body()
  device.unlockForConfiguration()
}

// a duration in seconds as the check messages print it: 4 -> "4", 5.5 -> "5.5"
func secondsText(_ seconds: Double) -> String { String(format: "%g", seconds) }
// Runs `body` on the main thread (for UIKit and the preview layer) and returns its result.
func onMain<T>(_ body: () -> T) -> T { Thread.isMainThread ? body() : DispatchQueue.main.sync(execute: body) }

// ---- capture formats
func dimensions(_ format: AVCaptureDevice.Format) -> CMVideoDimensions {
  CMVideoFormatDescriptionGetDimensions(format.formatDescription)
}

func is1080p(_ format: AVCaptureDevice.Format) -> Bool {
  let size = dimensions(format)
  return size.width == 1920 && size.height == 1080
}

func reaches30FPS(_ format: AVCaptureDevice.Format) -> Bool {
  format.videoSupportedFrameRateRanges.contains { $0.maxFrameRate >= 30 }
}

// the format's largest photo size, by pixel count
func largestPhotoDimensions(_ format: AVCaptureDevice.Format) -> CMVideoDimensions? {
  format.supportedMaxPhotoDimensions.max(by: { $0.width * $0.height < $1.width * $1.height })
}
