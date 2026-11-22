// SPDX-License-Identifier: Apache-2.0
// CaptureControl.swift: opening, closing and restarting capture as readers come and go, the camera is unplugged and
// plugged back in, and the Mac wakes.
import AVFoundation

extension Lifecycle {
  var sourceName: String { options.synthetic ? "synthetic source" : "camera" }

  // An active reader appeared: start the synthetic source, or the camera if it is connected.
  func openCapture() {
    wantOpen = true
    openSince = sch_now_ns()
    log("opening the \(sourceName) (active reader)")

    // For the camera, the state goes through camera.queue, so it lands after the idle state of a close still queued
    // there.
    if options.synthetic {
      ring.setState(RingState.starting)
    } else {
      camera.queue.async { self.ring.setState(RingState.starting) }
    }

    if options.synthetic && !unpluggedByTestHook {
      ring.queue.async { self.syntheticSource.start() }
      return
    }

    // A synthetic source or camera unplugged by --test-hooks, or a camera that is absent or not connected: wait for it.
    guard !options.synthetic, let device = device ?? AVCaptureDevice(uniqueID: uniqueID), device.isConnected,
          !unpluggedByTestHook else {
      disconnected = true
      log("camera \(uniqueID) is not connected; waiting for it")
      return
    }

    self.device = device
    disconnected = false
    camera.queue.async {
      if !self.camera.start(device) {
        log("camera start failed; retrying at the liveness check")
      }
    }
  }

  // No active reader for --close-after seconds.
  func closeCapture(_ why: String) {
    wantOpen = false
    log("closing the \(sourceName) (\(why))")
    stopCaptureThenClear(state: RingState.idle)
  }

  // Stops the synthetic source or the camera session, then clears the ring's slots and sets its state: no frame
  // outlives the capture in shared memory.
  func stopCaptureThenClear(state: UInt32) {
    let clear = {
      self.ring.queue.async {
        self.ring.clearSlots()
        self.ring.setState(state)
      }
    }

    if options.synthetic {
      ring.queue.async { self.syntheticSource.stop() }
      clear()
    } else {
      camera.queue.async {
        self.camera.stop()
        clear()
      }
    }
  }

  // A new session (or synthetic timer) while capture should run: after a runtime error, a wake, or frames stopping.
  func restartCapture(_ why: String) {
    guard wantOpen, !quitting else { return }
    log("restarting capture (\(why))")
    openSince = sch_now_ns()
    if options.synthetic {
      if !unpluggedByTestHook {
        ring.queue.async {
          self.syntheticSource.stop()
          self.syntheticSource.start()
        }
      }

      return
    }

    guard let device = AVCaptureDevice(uniqueID: uniqueID), device.isConnected, !unpluggedByTestHook else {
      disconnected = true
      log("camera not connected; waiting for it")
      return
    }

    self.device = device
    camera.queue.async {
      if !self.camera.start(device) {
        log("camera restart failed")
      }
    }
  }

  @objc func deviceDisconnected(_ notification: Notification) {
    guard let device = notification.object as? AVCaptureDevice, device.uniqueID == uniqueID else { return }
    cameraGone()
  }

  @objc func deviceConnected(_ notification: Notification) {
    guard let device = notification.object as? AVCaptureDevice, device.uniqueID == uniqueID else { return }
    cameraBack(device)
  }

  // The camera went away: capture stops and, as at any close, its frames leave shared memory.
  func cameraGone() {
    log("camera disconnected")
    disconnected = true
    stopCaptureThenClear(state: RingState.starting)
  }

  // The camera is back (device is nil for a --test-hooks reconnect): capture restarts if a reader wants it.
  func cameraBack(_ device: AVCaptureDevice?) {
    log("camera connected again")
    if let device = device {
      self.device = device
    }

    disconnected = false
    guard wantOpen else { return }
    openSince = sch_now_ns()
    if options.synthetic {
      ring.queue.async { self.syntheticSource.start() }
    } else if let device = self.device {
      camera.queue.async { _ = self.camera.start(device) }  // start logs why it fails
    }
  }

  // --test-hooks: SIGUSR1 (plugged false) and SIGUSR2 (plugged true)
  func simulatePlug(_ plugged: Bool) {
    log("test hook: simulated \(plugged ? "reconnect" : "disconnect")")
    unpluggedByTestHook = !plugged
    if plugged {
      cameraBack(nil)
    } else {
      cameraGone()
    }
  }

  @objc func didWake(_ notification: Notification) {
    log("Mac woke")
    restartCapture("wake")
  }
}
