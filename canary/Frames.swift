// SPDX-License-Identifier: Apache-2.0
// Frame checks: time to the first frame, a window of frames, and the preview layer.
import UIKit
import AVFoundation

// One delivered video frame. The presentation time and the arrival time are host-clock seconds. The signature comes
// from frameSignature, and nonzero says whether any sampled byte was nonzero.
struct FrameRecord {
  let pts: Double
  let arrival: Double
  let width: Int
  let height: Int
  let pixelFormat: OSType
  let signature: UInt64
  let nonzero: Bool
}

// A frame window: how long frames are collected after resetFrames(), and how many must arrive in it (fewer is a timing
// miss). The counts leave a wide margin below 30 fps.
struct FrameWindow {
  let seconds: Double
  let minFrames: Int
}

// a frame that misses the first-frame bound is waited for this much longer before it counts as never arriving
let firstFrameLateS = 10.0
// a frame's arrival minus its presentation time must be in [hostClockLagMinS, hostClockLagMaxS) seconds: frames stamped
// on the host clock arrive just after their stamp
let hostClockLagMinS = -0.05
let hostClockLagMaxS = 0.5
// the frame rate is checked only over a window longer than minFPSSpanS
let minFPS = 10
let minFPSSpanS = 0.3

extension Canary {
  // The time to the first frame is its own check, with an explicit bound, and the frame-rate windows start only once
  // frames flow. Fixed windows right after startRunning failed on a cold first launch and after foregrounding, though
  // frames then arrived normally: the first frame came 1.8 s after the process started on a virtual runner, and about
  // 7 s after it when a simulator had just booted on the Mac. CANARY_FIRST_FRAME_S overrides the bound (default 5 s).
  func firstFrame(_ label: String) {
    let limit = Config.firstFrameLimit()
    let start = Date()
    resetFrames()
    let ok = waitFor(limit) { !self.snapshotFrames().isEmpty }
    check(ok, timing: true,
          "\(label) first frame within \(limit) s (took \(String(format: "%.2f", Date().timeIntervalSince(start))) s)")
    if !ok, !waitFor(firstFrameLateS, { !self.snapshotFrames().isEmpty }) {   // late is timing; never is a FAIL
      check(false, "\(label) no frame at all (\(limit + firstFrameLateS) s)")
    }
  }

  // Collects frames for the window's length, then checks them.
  // Some frames must arrive: none is a FAIL. Too few is a timing miss, and so is a rate under minFPS.
  // Each frame must have an image, a rising host-clock timestamp and the expected size, and the content must be
  // nonzero and changing.
  func checkFrameWindow(_ label: String, _ window: FrameWindow, expectedWidth: Int? = nil, expectedHeight: Int? = nil) {
    resetFrames()
    pause(window.seconds)
    let minCount = window.minFrames
    let received = snapshotFrames()

    // none at all is a FAIL; too few is timing
    check(!received.isEmpty, "\(label) any frame in the window (dropped=\(dropped))")
    if !received.isEmpty {
      check(received.count >= minCount, timing: true,
            "\(label) frames=\(received.count) (>= \(minCount) in window) dropped=\(dropped)")
    }

    guard received.count >= 2, let first = received.first, let last = received.last else { return }
    check(received.allSatisfy { $0.pts > 0 && $0.width > 0 }, "\(label) every frame has image buffer + valid pts")
    let rising = !zip(received, received.dropFirst()).contains { $1.pts <= $0.pts }
    check(rising, "\(label) pts strictly increasing")
    let lag = received.map { $0.arrival - $0.pts }
    let lagMin = String(format: "%.3f", lag.min() ?? 0)
    let lagMax = String(format: "%.3f", lag.max() ?? 0)
    check(lag.allSatisfy { $0 >= hostClockLagMinS && $0 < hostClockLagMaxS },
          "\(label) pts on host clock (arrival-pts in [\(lagMin), \(lagMax)])")
    check(received.allSatisfy { $0.nonzero }, "\(label) pixels not all zero")
    let distinct = Set(received.map { $0.signature }).count
    check(distinct > received.count / 2,
          "\(label) content changes frame to frame (\(distinct)/\(received.count) distinct)")
    if let width = expectedWidth, let height = expectedHeight {
      check(last.width == width && last.height == height,
            "\(label) dims \(last.width)x\(last.height) == \(width)x\(height)")
    }

    let span = last.pts - first.pts
    if span > minFPSSpanS {
      let fps = Double(received.count - 1) / span
      check(fps >= Double(minFPS), timing: true, "\(label) fps \(String(format: "%.1f", fps)) (>= \(minFPS))")
    }
  }

  // whether the preview layer, or any layer under it, has contents (a frame was drawn)
  func previewHasContents() -> Bool {
    onMain {
      func walk(_ layer: CALayer) -> Bool {
        if layer.contents != nil { return true }
        return (layer.sublayers ?? []).contains(where: walk)
      }

      return preview.map(walk) ?? false
    }
  }
}
