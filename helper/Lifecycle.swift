// SPDX-License-Identifier: Apache-2.0
// Lifecycle.swift: the app delegate. It sets the helper up when the app has launched, handles the signals that stop
// it, and retires the ring when it quits. Permission.swift, Readers.swift and CaptureControl.swift extend it.
import AppKit
import AVFoundation

// The helper's state machine: whether capture should run, and the timers and observers that drive it.
//
// Threading: every mutable property belongs to the main queue, and every method runs there. Work on the camera goes
// through camera.queue, and work on the ring and the synthetic source through ring.queue.
final class Lifecycle: NSObject, NSApplicationDelegate {
  // the ring's heartbeat, which tells readers a live helper from a dead one
  static let heartbeatMs = 100
  // the reader scan runs first this long after launch, then at this interval (and at once when a reader file changes)
  static let readerScanFirstS = 0.2
  static let readerScanEveryS = 1.0
  // after permission is denied, the helper exits this much later (the pid file says denied meanwhile)
  static let deniedExitS = 3.0

  let options: Options
  let uniqueID: String
  let readersDir: String
  let pidFile: PidFile
  let ring: RingWriter
  let camera: Camera
  let syntheticSource: SyntheticSource

  // the camera to open; nil for the synthetic source, or until a camera named by --camera shows up
  var device: AVCaptureDevice?
  // A reader is active, so the camera (or the synthetic source) should run.
  var wantOpen = false
  // when capture was last opened or restarted (host clock, ns)
  var openSince: UInt64 = 0
  var disconnected = false
  // when an active reader, and any reader, was last seen (host clock, ns)
  var lastActiveNs = sch_now_ns()
  var lastReaderNs = sch_now_ns()
  // the reader list last logged, so that it is logged only when it changes
  var readersLogged = ""
  // --test-hooks: SIGUSR1 unplugs the camera and SIGUSR2 plugs it back in. The device itself stays connected; this
  // flag hides it.
  var unpluggedByTestHook = false
  var quitting = false
  private var readerScanTimer: DispatchSourceTimer?
  private var readersDirWatch: DispatchSourceFileSystemObject?
  private var heartbeatTimer: DispatchSourceTimer?
  private var signalSources: [DispatchSourceSignal] = []

  init(options: Options, choice: CameraChoice, readersDir: String, pidFile: PidFile, ring: RingWriter) {
    self.options = options
    self.uniqueID = choice.uniqueID
    self.device = choice.device
    self.readersDir = readersDir
    self.pidFile = pidFile
    self.ring = ring
    camera = Camera(ring: ring, limits: options.formatLimits)
    syntheticSource = SyntheticSource(ring: ring, options: options)
    super.init()
    camera.onRuntimeError = { [unowned self] in self.restartCapture("runtime error") }
  }

  func applicationDidFinishLaunching(_ notification: Notification) {
    installSignalHandlers()

    // The ring exists from the start, so readers see why it is offline: starting, prompting, idle or denied.
    guard createFirstRing() else {
      pidFile.write()
      log("no ring; exiting")
      quit("ring create failed", code: 1)
      return
    }

    pidFile.write()
    startHeartbeat()
    observeDevices()
    settlePermission()
    startReaderScan()
  }

  func installSignalHandlers() {
    let stopSignals: [(number: Int32, name: String)] = [(SIGTERM, "SIGTERM"), (SIGINT, "SIGINT"), (SIGHUP, "SIGHUP")]
    for stopSignal in stopSignals {
      handleSignal(stopSignal.number) { [unowned self] in self.quit(stopSignal.name, code: 0) }
    }

    if options.testHooks {
      for plugSignal in [SIGUSR1, SIGUSR2] {
        handleSignal(plugSignal) { [unowned self] in self.simulatePlug(plugSignal == SIGUSR2) }
      }
    }
  }

  // The signal's default action is turned off; a dispatch source on the main queue runs the handler instead.
  func handleSignal(_ number: Int32, _ handler: @escaping () -> Void) {
    signal(number, SIG_IGN)
    let source = DispatchSource.makeSignalSource(signal: number, queue: .main)
    source.setEventHandler { handler() }
    source.resume()
    signalSources.append(source)
  }

  // The first ring object, at the size of the camera's best format (--size for the synthetic source).
  func createFirstRing() -> Bool {
    var width = options.maxWidth
    var height = options.maxHeight
    if let device = device, let format = options.formatLimits.bestFormat(of: device) {
      let size = dimensions(format)
      width = UInt32(size.width)
      height = UInt32(size.height)
    }

    return ring.queue.sync { ring.create(width: width, height: height) }
  }

  func startHeartbeat() {
    let timer = DispatchSource.makeTimerSource(queue: ring.queue)
    timer.schedule(deadline: .now(), repeating: .milliseconds(Self.heartbeatMs))
    timer.setEventHandler { [ring] in ring.beat() }
    timer.resume()
    heartbeatTimer = timer
  }

  func observeDevices() {
    let center = NotificationCenter.default
    center.addObserver(self, selector: #selector(deviceDisconnected(_:)),
                       name: AVCaptureDevice.wasDisconnectedNotification, object: nil)
    center.addObserver(self, selector: #selector(deviceConnected(_:)),
                       name: AVCaptureDevice.wasConnectedNotification, object: nil)
    NSWorkspace.shared.notificationCenter.addObserver(self, selector: #selector(didWake(_:)),
                                                      name: NSWorkspace.didWakeNotification, object: nil)
  }

  // The reader scan (Readers.swift) runs on a timer, and at once when a reader file is written.
  func startReaderScan() {
    let timer = DispatchSource.makeTimerSource(queue: .main)
    timer.schedule(deadline: .now() + Self.readerScanFirstS, repeating: Self.readerScanEveryS)
    timer.setEventHandler { [unowned self] in self.tick() }
    timer.resume()
    readerScanTimer = timer
    let dirFD = open(readersDir, O_EVTONLY)
    if dirFD >= 0 {
      let watch = DispatchSource.makeFileSystemObjectSource(fileDescriptor: dirFD, eventMask: .write, queue: .main)
      watch.setEventHandler { [unowned self] in self.tick() }
      watch.setCancelHandler { close(dirFD) }
      watch.resume()
      readersDirWatch = watch
    }
  }

  // Retires the helper: stops capture, marks the ring stopped, clears its slots, unlinks its name and removes the pid
  // file, then exits. Only the first call does anything.
  func quit(_ why: String, code: Int32) {
    if quitting { return }
    quitting = true
    log("quitting (\(why))")
    readerScanTimer?.cancel()
    readersDirWatch?.cancel()
    if !options.synthetic {
      camera.queue.sync { camera.stop() }
    }

    ring.queue.sync {
      syntheticSource.stop()
      ring.retire(why)
    }

    pidFile.state = RingState.stopped

    // still under the lock, so no new helper's file is removed
    unlink(pidFile.path)
    log("exit \(code)")
    exit(code)
  }

  func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
    quit("terminate", code: 0)
    return .terminateNow
  }
}
