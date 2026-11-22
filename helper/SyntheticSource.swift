// SPDX-License-Identifier: Apache-2.0
// SyntheticSource.swift: the --synthetic source, a moving test pattern (shim.h's sch_synth_draw) with the latency
// stamp, for tests that need no camera and no permission.
import Foundation

// A timer on the ring's queue that publishes one pattern frame per --fps interval, at the --size frame size.
//
// Threading: every property belongs to the ring's queue, and start and stop must be called on it.
final class SyntheticSource {
  // how late the frame timer may fire
  static let timerLeeway = DispatchTimeInterval.microseconds(200)

  private let ring: RingWriter
  private let width: UInt32
  private let height: UInt32
  private let fps: Double
  private let jitterMs: Double
  private var timer: DispatchSourceTimer?
  private var pattern = sch_synth()
  private var frameIndex: UInt64 = 0

  init(ring: RingWriter, options: Options) {
    self.ring = ring
    width = options.maxWidth
    height = options.maxHeight
    fps = options.fps
    jitterMs = options.jitterMs
  }

  // Starts the timer, first making a ring object of the pattern's size if the current one has another size. Does
  // nothing while the timer runs.
  func start() {
    if timer != nil { return }
    if !ring.hasObject || ring.width != width || ring.height != height {
      guard ring.create(width: width, height: height) else { return }
    }

    ring.capturing = true
    let timer = DispatchSource.makeTimerSource(flags: .strict, queue: ring.queue)
    timer.schedule(deadline: .now(), repeating: .nanoseconds(Int(1e9 / fps)), leeway: Self.timerLeeway)
    timer.setEventHandler { [unowned self] in self.publishFrame() }
    timer.resume()
    self.timer = timer
    let jitterNote = jitterMs > 0 ? ", publish jitter 0-\(jitterMs) ms" : ""
    log("synthetic source on (\(width)x\(height)@\(fps)\(jitterNote))")
  }

  func stop() {
    timer?.cancel()
    timer = nil
    ring.capturing = false
  }

  // One frame: stamped with the time the timer fired, then published after the --jitter-ms delay, if any.
  private func publishFrame() {
    let captureNs = sch_now_ns()
    if jitterMs > 0 {
      usleep(useconds_t(Double.random(in: 0...jitterMs) * 1000))
    }

    ring.publish(captureNs: captureNs, flags: UInt32(SCW_SLOT_SYNTHETIC)) { pixels in
      let bytesPerRow = ring.width * RingWriter.bytesPerPixel
      sch_synth_draw(&pattern, pixels, bytesPerRow, ring.width, ring.height, frameIndex)
    }

    frameIndex += 1
  }
}
