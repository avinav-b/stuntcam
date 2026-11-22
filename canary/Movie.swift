// SPDX-License-Identifier: Apache-2.0
// A 1.5 s recording on the movie file output: it starts, finishes in time, and its video track has the active
// format's size and the right duration.
import AVFoundation

// the recording finishes within movieFinishS; never, if not after movieLateDrainS more
let movieFinishS = 4.0
let movieLateDrainS = 8.0
// how long reading the recorded file's duration and tracks may take
let movieLoadTimeoutS = 3.0
// how long the recording may take to start, and how long it runs
let recordingStartWaitS = 1.5
let recordingLengthS = 1.5
// the recorded file's duration must lie strictly between these (seconds)
let movieMinDurationS = 1.0
let movieMaxDurationS = 3.0

// the smallest format that is not 1920x1080 and runs at 30 fps or more
func smallestNon1080Format(_ device: AVCaptureDevice) -> AVCaptureDevice.Format? {
  func pixels(_ format: AVCaptureDevice.Format) -> Int {
    let size = dimensions(format)
    return Int(size.width) * Int(size.height)
  }
  return device.formats.filter { !is1080p($0) && reaches30FPS($0) }.min { pixels($0) < pixels($1) }
}

// the recorded file's duration (s) and its video tracks' sizes ("WxH", or "err …"), as far as they loaded within
// movieLoadTimeoutS
func loadMovie(_ url: URL) -> (duration: Double, videoTracks: [String]) {
  let asset = AVURLAsset(url: url)
  let loaded = DispatchSemaphore(value: 0)
  var duration = 0.0
  var videoTracks: [String] = []
  Task {
    do {
      duration = try await asset.load(.duration).seconds
      for track in try await asset.loadTracks(withMediaType: .video) {
        let size = try await track.load(.naturalSize)
        videoTracks.append("\(Int(size.width))x\(Int(size.height))")
      }
    } catch { videoTracks.append("err \(error)") }
    loaded.signal()
  }
  _ = loaded.wait(timeout: .now() + movieLoadTimeoutS)  // a timeout returns what loaded: the track check reports a miss
  return (duration, videoTracks)
}

extension Canary {
  // Records on a format other than 1080p and compares the track with the active format: 1080p is also the
  // engine's fallback size, so a recording that ignored the active format passed a mutation run.
  func recordMovie(_ device: AVCaptureDevice, _ movie: AVCaptureMovieFileOutput) {
    verdict.stage = "movie"
    if let format = smallestNon1080Format(device) {
      try? withConfiguration(device) { device.activeFormat = format }  // unlocked: activeSize reads back the old format
      pause(formatSettleS)
    }

    let activeSize = dimensions(device.activeFormat)
    let name = "canary-\(Int(Date().timeIntervalSince1970)).mov"
    let url = FileManager.default.temporaryDirectory.appendingPathComponent(name)
    try? FileManager.default.removeItem(at: url)  // usually there is none
    movie.startRecording(to: url, recordingDelegate: self)
    check(waitFor(recordingStartWaitS) { eventCount("movieStarted") > 0 }, "didStartRecording")
    pause(recordingLengthS)
    let recordedDuration = movie.recordedDuration.seconds
    movie.stopRecording()
    waitForRecordingFinish()
    guard let (recordedURL, recordingError) = movieDone else { return }
    check(recordingError == nil, "recording error \(String(describing: recordingError))")
    let (duration, videoTracks) = loadMovie(recordedURL)
    check(videoTracks.contains("\(activeSize.width)x\(activeSize.height)")
            || videoTracks.contains("\(activeSize.height)x\(activeSize.width)"),
          "movie track \(videoTracks) matches the active format \(activeSize.width)x\(activeSize.height)")
    let durationText = String(format: "%.2f", duration)
    let recordedDurationText = String(format: "%.2f", recordedDuration)
    check(duration > movieMinDurationS && duration < movieMaxDurationS && !videoTracks.isEmpty,
          "movie duration \(durationText) s video tracks \(videoTracks) recordedDuration \(recordedDurationText)")
  }

  // late is a timing miss; never is a FAIL
  func waitForRecordingFinish() {
    let finished = waitFor(movieFinishS) { lock.withLock { movieDone != nil } }
    check(finished, timing: true, "didFinishRecording within \(secondsText(movieFinishS)) s")
    if !finished, !waitFor(movieLateDrainS, { lock.withLock { movieDone != nil } }) {
      check(false, "recording never finished (\(secondsText(movieFinishS + movieLateDrainS)) s)")
    }
  }
}
