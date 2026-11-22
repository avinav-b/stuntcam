// SPDX-License-Identifier: Apache-2.0
// main.swift: Stuntcam Camera, the camera helper. It publishes one Mac camera, or a synthetic pattern, into the
// shared-memory ring that the engine's `webcam` source reads. docs/webcam-ring.md is the ring contract, and
// Options.swift lists the command line.
//
// The helper is launched only through LaunchServices, so that it is its own responsible process for camera permission:
//   open -n -g "<absolute path>/Stuntcam Camera.app" --args --home "$SIMCAM_HOME" [options]
//
// This file is the launch sequence. It runs in order: the options, the camera, the lock, then the app (Lifecycle) on
// the main queue. The values it sets up here never change afterwards. Swift initializes main.swift's globals as this
// code reaches them, and every other file's globals lazily on first use, so everything that has to happen at a fixed
// moment of the launch happens here.
//
// The other files, one concern each:
//   Options.swift         the command line
//   Files.swift           the directories and file names under $SIMCAM_HOME, and the bundle's <sha>
//   Discovery.swift       finding the cameras, the default camera, --list
//   Log.swift             the log file
//   PidFile.swift         the pid file (main queue)
//   RingWriter.swift      the ring object and the frames written into it (ring queue)
//   SyntheticSource.swift the --synthetic pattern (ring queue)
//   Camera.swift          the capture session (session queue) and the capture format
//   Lifecycle.swift       the app delegate: launch, signals, quit (main queue)
//   Permission.swift      the camera permission, settled at launch
//   Readers.swift         the reader files, the reader scan and the liveness check
//   CaptureControl.swift  opening, closing and restarting capture as readers and the camera come and go
import AppKit
import AVFoundation
import Darwin

let options = Options.parse(CommandLine.arguments.dropFirst())
guard options.home.hasPrefix("/") else { fail("--home <absolute SIMCAM_HOME> is required", code: 2) }
let directories = Directories(home: options.home)
directories.create()
let startedUnix = UInt64(Date().timeIntervalSince1970)
if let listPath = options.listPath { listCameras(to: listPath) }

// The camera is chosen before anything is locked: its uniqueID names the lock, the pid file and the ring.
let choice = CameraChoice.choose(options)
let files = CameraFiles(home: options.home, uniqueID: choice.uniqueID, webcamDir: directories.webcam)
let sha = bundleSHA(options.sha)

let logFile = LogFile(path: directories.logs + "/webcam-\(files.cam).log")

// The lock: one helper per camera and $SIMCAM_HOME.
let lockFD = sch_lock(files.lockPath)
if lockFD < 0 {
  log("another helper holds \(files.lockPath) (\(String(cString: strerror(errno)))); exiting")
  exit(0)
}
log("start \(Bundle.main.bundleIdentifier ?? "-") sha=\(sha) camera=\(choice.uniqueID) '\(choice.name)'"
    + " cam=\(files.cam) shm=\(files.shmName) synthetic=\(options.synthetic)"
    + " size<=\(options.maxWidth)x\(options.maxHeight)@\(options.fps)"
    + " close-after=\(options.closeAfterS)s quit-after=\(options.quitAfterS)s"
    + " path=\(Bundle.main.bundlePath)")
for line in choice.discoveryLog {
  log(line)
}

// A synthetic helper never names the default camera.
if options.camera == nil && !options.synthetic {
  writeDefaultCamera(choice.uniqueID, webcamDir: directories.webcam)
}

let pidFile = PidFile(path: files.pidPath, sha: sha, uniqueID: choice.uniqueID, startedUnix: startedUnix)
let ring = RingWriter(shmName: files.shmName, cameraName: choice.name, fps: options.fps,
                      stampsFrames: options.stampFrames || options.synthetic, pidFile: pidFile)
let lifecycle = Lifecycle(options: options, choice: choice, readersDir: directories.readers, pidFile: pidFile,
                          ring: ring)
let app = NSApplication.shared
app.delegate = lifecycle
app.setActivationPolicy(.accessory)
app.run()
