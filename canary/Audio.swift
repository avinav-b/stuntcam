// SPDX-License-Identifier: Apache-2.0
// Extended: microphone input + AVCaptureAudioDataOutput deliver buffers on the host clock.
import AVFoundation

// the session runs this long, and must deliver at least minAudioBuffers buffers meanwhile
let audioRunS = 1.0
let minAudioBuffers = 20
// every buffer's timestamp is in the past on the host clock, by less than this
let audioMaxAgeS = 3.0

extension Canary {
  func audio() {
    verdict.stage = "audio"
    guard let mic = AVCaptureDevice.default(for: .audio) else {
      check(false, "microphone device present", ext: true)
      return
    }

    let session = AVCaptureSession()
    session.beginConfiguration()
    guard let micInput = try? AVCaptureDeviceInput(device: mic), session.canAddInput(micInput) else {
      check(false, "mic input", ext: true)
      return
    }

    session.addInput(micInput)
    let audioOutput = AVCaptureAudioDataOutput()
    audioOutput.setSampleBufferDelegate(self, queue: DispatchQueue(label: "canary.ado"))
    session.addOutput(audioOutput)
    session.commitConfiguration()
    session.startRunning()
    pause(audioRunS)
    session.stopRunning()
    let (bufferCount, timestamps, rate) = lock.withLock { (audioBufferCount, audioPTS, audioRate) }
    check(bufferCount >= minAudioBuffers, "audio buffers in \(secondsText(audioRunS)) s: \(bufferCount) rate \(rate)",
          ext: true)
    let now = hostNow()
    let onHostClock = timestamps.allSatisfy { now - $0 < audioMaxAgeS && now - $0 > 0 }
    check(!timestamps.isEmpty && onHostClock, "audio pts on host clock", ext: true)
  }
}
