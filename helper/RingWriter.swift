// SPDX-License-Identifier: Apache-2.0
// RingWriter.swift: the shared-memory ring the helper publishes frames into. docs/webcam-ring.md describes the object,
// its slots and states, and how the helper creates and retires it; the C side is engine/src/webcam/ring.c and
// ring_shm.c.
import Foundation

// The ring states (docs/webcam-ring.md, "States") in the form the C writer and the pid file take.
enum RingState {
  static let starting = UInt32(SCW_STATE_STARTING)
  static let prompting = UInt32(SCW_STATE_PROMPTING)
  static let running = UInt32(SCW_STATE_RUNNING)
  static let idle = UInt32(SCW_STATE_IDLE)
  static let denied = UInt32(SCW_STATE_DENIED)
  static let stopped = UInt32(SCW_STATE_STOPPED)
  static let superseded = UInt32(SCW_STATE_SUPERSEDED)

  static func name(_ state: UInt32) -> String {
    String(cString: scw_state_name(state))
  }
}

// What the frame path counted since the last reader scan, which logs it and starts over.
struct FrameStats {
  // frames published, and the time spent filling their slots
  var published = 0
  var copyNs: UInt64 = 0
  // camera frames received, and the sum of their capture-to-arrival delays
  var cameraFrames = 0
  var delayNs: Int64 = 0
}

// The ring object at the helper's shm name, its geometry, and the frames written into it.
//
// Threading: every property belongs to `queue`, and every method except setState must be called on it. The camera's
// sample buffer delegate and the synthetic source's timer run on `queue`, so frames need no lock.
final class RingWriter {
  // the ring version this helper publishes: part of the shm name, and what readers must accept (the pid file's rings=1
  // says the same)
  static let version: UInt32 = 1
  // 32-bit BGRA pixels in 4 slots
  static let bytesPerPixel: UInt32 = 4
  static let slotCount: UInt32 = 4

  let queue = DispatchQueue(label: "dev.stuntcam.camera.ring", qos: .userInteractive)
  // Frames are accepted. Camera.start and SyntheticSource.start set it; their stop and retire() clear it.
  var capturing = false
  private var stats = FrameStats()
  private(set) var width: UInt32 = 0
  private(set) var height: UInt32 = 0
  // when the last frame was published (host clock, ns), for the liveness check
  private(set) var lastFrameNs: UInt64 = 0
  private var writer: UnsafeMutablePointer<scw_shm_writer>?
  // the last frame's sequence number; it starts from 1 in each ring object
  private var seq: UInt64 = 0
  private var state = RingState.starting

  private let shmName: String
  private let cameraName: String
  private let fps: Double
  private let stampsFrames: Bool
  private let pidFile: PidFile

  init(shmName: String, cameraName: String, fps: Double, stampsFrames: Bool, pidFile: PidFile) {
    self.shmName = shmName
    self.cameraName = cameraName
    self.fps = fps
    self.stampsFrames = stampsFrames
    self.pidFile = pidFile
  }

  var hasObject: Bool { writer != nil }

  // The ring state, from any queue: set in the ring object on `queue`, then recorded in the pid file on the main queue.
  func setState(_ newState: UInt32) {
    queue.async {
      self.state = newState
      if let writer = self.writer {
        scw_writer_state(sch_ring(writer), newState)
      }

      DispatchQueue.main.async {
        self.pidFile.update(state: newState)
      }
    }
  }

  // A new object at the name, with a new epoch and seq from 1. It is published before the old one is retired as
  // superseded, so readers always find one. False (logged) when the geometry is out of range or the object can't be
  // made.
  func create(width: UInt32, height: UInt32) -> Bool {
    let bytesPerRow = width * Self.bytesPerPixel
    let bytes = scw_object_bytes(height, bytesPerRow, Self.slotCount, nil)
    guard bytes > 0 else {
      log("ring: geometry \(width)x\(height) out of range")
      return false
    }

    let newWriter = UnsafeMutablePointer<scw_shm_writer>.allocate(capacity: 1)
    newWriter.initialize(to: scw_shm_writer())
    var report = scw_shm_report()
    let createResult = scw_shm_create(newWriter, shmName, bytes, &report)
    if createResult != SCW_OK {
      log("ring: \(String(cString: sch_report_msg(&report))) (rc \(createResult))")
      newWriter.deallocate()
      return false
    }

    let epoch = sch_epoch()
    let fpsMillis = UInt32(fps * 1000)
    let initResult = scw_writer_init(sch_ring(newWriter), UnsafeMutableRawPointer(newWriter.pointee.header),
                                     newWriter.pointee.len, width, height, bytesPerRow, Self.slotCount, epoch,
                                     fpsMillis, getpid(), cameraName)
    if initResult != SCW_OK {
      log("ring: writer init \(initResult)")
      scw_shm_retire(newWriter, RingState.stopped)
      newWriter.deallocate()
      return false
    }

    scw_writer_state(sch_ring(newWriter), state)
    scw_writer_beat(sch_ring(newWriter), sch_now_ns())
    scw_writer_publish(sch_ring(newWriter))
    if let old = writer {
      scw_shm_retire(old, RingState.superseded)
      old.deallocate()
    }

    writer = newWriter
    self.width = width
    self.height = height
    seq = 0
    log(String(format: "ring %@ %ux%u BGRA, %u slots, %llu bytes, epoch %016llx, mode %04o (fchmod errno %d)", shmName,
               width, height, Self.slotCount, bytes, epoch, report.mode, report.fchmodErrno))
    return true
  }

  // Stops accepting frames and retires the object: state stopped, slots cleared, name unlinked.
  func retire(_ why: String) {
    capturing = false
    if let writer = writer {
      scw_shm_retire(writer, RingState.stopped)
      writer.deallocate()
      self.writer = nil
      log("ring retired (\(why)): slots cleared, name unlinked")
    }
  }

  // One frame. `fill` writes the slot's pixels, width * bytesPerPixel bytes per row.
  func publish(captureNs: UInt64, flags: UInt32, fill: (UnsafeMutablePointer<UInt8>) -> Void) {
    guard capturing, let writer = writer else { return }
    let fillStart = sch_now_ns()
    seq += 1
    guard let pixels = scw_writer_begin(sch_ring(writer), seq) else { return }
    fill(pixels)
    if stampsFrames {
      sch_stamp(pixels, width * Self.bytesPerPixel, width, height, seq, captureNs)
    }

    let publishNs = sch_now_ns()
    scw_writer_end(sch_ring(writer), seq, captureNs, publishNs, flags)
    lastFrameNs = publishNs
    stats.published += 1
    stats.copyNs += publishNs - fillStart
    if state != RingState.running {
      setState(RingState.running)
    }
  }

  // one camera frame received, delayNs after its capture time
  func countCameraFrame(delayNs: Int64) {
    stats.cameraFrames += 1
    stats.delayNs += delayNs
  }

  // the heartbeat that tells readers the helper is alive
  func beat() {
    if let writer = writer {
      scw_writer_beat(sch_ring(writer), sch_now_ns())
    }
  }

  // Clears every slot, so that no frame outlives the capture that made it.
  func clearSlots() {
    if let writer = writer {
      scw_writer_clear(sch_ring(writer))
    }
  }

  // Logs what the frame path counted since the last call, then starts over.
  func logStatsAndReset() {
    if stats.published > 0 || stats.cameraFrames > 0 {
      let copyMs = Double(stats.copyNs) / Double(max(stats.published, 1)) / 1e6
      let delayMs = Double(stats.delayNs) / Double(max(stats.cameraFrames, 1)) / 1e6
      log(String(format: "stat published %d (seq %llu) copy %.2f ms camera->helper %.1f ms", stats.published, seq,
                 copyMs, delayMs))
    }

    stats = FrameStats()
  }
}
