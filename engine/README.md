# engine

The in-app library (`libsimcam.dylib`) and the boot stub. Build with `../build.sh`.

- `stub/stub.c`: the frozen boot stub (libSystem only, inserted at boot into every sim process). In app processes it
  reads `$SIMCAM_SIM_DIR/current` (`<proto> <loader path>`) and dlopens that loader; rewriting `current`
  (`scripts/swap.sh`) changes the build for the next launch without a reboot.
- `loader/loader.c`: `libsimcamloader.dylib`, built into the same hash dir as the engine. Restores the libraries
  Xcode/XCTest would have inserted, dlopens the engine next to it (not in `*-Runner.app` or `.appex`), then each library in
  the chain file.
- `runnerhook/runnerhook.m`: `libsimcamrunnerhook.dylib` (Foundation; its own hash dir). Never in a user app as such:
  `stuntcam exec -- xcodebuild test` inserts it into the XCTest runner on launchd-armed or unarmed sims, where it adds the
  stub and stuntcam's variables to each app a UI test launches, and puts back what that insert displaced from the runner
  (`libXCTestBundleInject`, MTC, RPAC: restore-rules `test-runner`). Inert on boot-armed sims.
- `src/simcam.m`: the engine's single translation unit. It includes the fragments below **in this order**: they share
  file-scope state and `__attribute__((constructor))` functions run in source order.

  | Fragment | What is in it |
  |---|---|
  | `core/env.inc` | the environment variables the engine reads, each a named constant with its meaning: settings, paths and camera settings the CLI sets when arming (these six also under their stuntcam 1.1.0 names, `ENV_LEGACY_*`, read with `EnvConfig`), tuning, diagnostics (`ENV_DIAG*`), off switches (`ENV_OFF_*`), test faults (`ENV_TEST_*`) |
  | `core/switches.inc` | the off switches and test faults read where they act, as named predicates (`PhotoDrainOn()`, `TestFaultLong(ENV_TEST_…)`): each call reads the environment, as the `getenv` it replaced did |
  | `fig/sources.inc` | the capture sources the simulator's capture layer asks for: the object ABI, the source model, attributes, formats, the source factories, the microphone source |
  | `core/act.inc` | activation's shared state: the state machine's states and phases, test-fault parsing, the hook table (resolve in PREPARE, install in SWAP) |
  | `core/preflight.inc` | runtime detection, the session vtable layout check, the preflight gate (all or nothing); also the Mach-O load-command walk the crash handler and the session hook share |
  | `core/safety.inc` | the op watchdog and crash sentinel: overview, shared state, the per-thread feature key, the signal-safe event writer, ops |
  | `core/crash_handler.inc` | the sentinel's signal handlers and the image ranges a crash is attributed against |
  | `core/markers.inc` | the sentinel's load-time state and its marker files (`latched-<build>`, `preflight-fail-<build>`…): paths, writes, prunes |
  | `core/events_file.inc` | the events file's writers outside the crash handler: trim, locked append, the record queue |
  | `core/sentinel_activate.inc` | the sentinel's activation steps: PREPARE's sentinel step (read the past, decide latches) and the sentinel's START step |
  | `core/trace.inc` | the op trace ring that `stuntcam trace` reads |
  | `core/init.inc` | process start, one engine per process, the log, PREPARE's lookups step, the source factories' resolve step |
  | `fig/session_state.inc` | per-session state and sink routing |
  | `providers/video.inc` | the frame source, per-sink conversion, preview frames |
  | `webcam/ring.c`, `webcam/ring_shm.c` | the webcam frame ring (docs/webcam-ring.md); the camera helper compiles the same files |
  | `providers/webcam.inc`, `providers/webcam_path.inc` | the Mac webcam source and its frame paths (GPU, VideoToolbox, CoreGraphics) |
  | `providers/scene.inc` | the generated scene (orientation watermark, QR, EAN-13), the virtual device orientation, switchable sources |
  | `providers/mirror.inc` | mirroring, and the notes the pump mirrors the preview by |
  | `providers/barcodes.inc` | barcode detection for the metadata sinks |
  | `providers/audio.inc` | movie audio and the synthetic audio source |
  | `providers/pump.inc` | the frame pump and the movie file sinks |
  | `providers/photo.inc` | photo encoding and the still-image slots |
  | `fig/session_hook.inc` | session adoption, teardown, the session-creation hook |
  | `client/*.inc` | small stock-first fixes on public classes where the simulator build compiles a path out: CoreMotion (`motion.inc`), photo data and the shutter trace (`photo.inc`), the rotation coordinator and preview rotation (`rotation.inc`), recommended output settings (`output_settings.inc`), the Vision barcode revision (`vision.inc`). Each has an off switch |
  | `core/control.inc` | the host control file, per-launch settings, virtual device orientation, interruptions |
  | `core/lazy.inc` | lazy arming, last: traps on the public capture entry points open the gate on the first camera call (PREPARE: sentinel, lookups, preflight, resolve; SWAP; START) |

  **Where state lives.** The fragments share one file scope, so any of them can read another's globals. To keep that
  visible, each fragment opens with a short interface block under its first line, one group per line:

  ```
  // Provides: the functions and constants other fragments use (the fragments that use them)
  // Owns:     the globals another fragment reads or writes, by group (who else writes them), and where they're defined
  // Uses:     the fragments whose functions, constants or globals this one uses
  ```

  A global that only its own fragment touches is not listed; inside a fragment, globals are grouped by concern under a
  comment that names the queue or thread that owns them. `core/env.inc`, `core/switches.inc` and simcam.m's `LOG` are
  used by every fragment and are left out of the Uses lines.

  **Subsystem headers.** Each fragment whose names other fragments use has a small internal header next to it. The
  header defines the state other fragments read or write (most of it one named struct per subsystem, whose fields it
  documents with the queue or lock that owns them), holds the constants they share, and declares the functions they
  call. It is still one translation unit and nothing is exported: a header's guard makes the first fragment that
  includes it compile it, and every later include a no-op. The "Owner" column names the fragment that defines what
  the header declares. A fragment includes the header of every fragment whose names it uses, so each name has a
  declaration in a file the fragment includes; fragments hold no forward declarations of each other's functions. Some headers hold only declarations: `client/rotation.h` (the rotation coordinator),
  `core/init.h` (process start), `client/photo.h` and `client/output_settings.h` (the resolve steps of those fixes),
  and `fig/sources.h`, which repeats what `fig/sources.inc` defines for the later fragments (that fragment doesn't
  include it). `providers/qr.c` is plain C shared with its host test and has no header.

  | Header | State | Owner |
  |---|---|---|
  | `core/activation.h` | `gActivation` (state machine, phase, budget, timing, queues, trigger, hook list), `gActivationThread`, the test switches parsed at load; the phases and hook groups | `core/act.inc` |
  | `core/preflight.h` | `gSessSlots`, the CoreMedia queue and session functions the lookups resolve; the session vtable's slots | `core/preflight.inc` |
  | `core/sentinel.h` | `gSentinel` (mode, build, state paths, load-time markers, events file, op slots, feature key), `gCrashHandler` (image ranges, saved actions, the motion marker), the marker test switches; the features, the latch constants, the events-file fields | `core/safety.inc` |
  | `core/trace.h` | `gTrace`'s type (op ring, trace file paths, the crash handler's claim and buffer); only `core/trace.inc` touches `gTrace`, so it is defined there; the ops | `core/trace.inc` |
  | `core/control.h` | `gControl`'s type (the poll, the control file read at PREPARE, what SWAP stores); only `core/control.inc` touches `gControl`, so it is defined there | `core/control.inc` |
  | `fig/session.h` | `gSessionLayer` (vtables, queues, session table), `gStopWait`, `LocalQueueMsg`; the classes `SimcamSink`, `SimcamRecording`, `SimcamSession` | `fig/session_state.inc` |
  | `fig/session_hook.h` | `gSessionHook` (the route, the session's base class with our finalize, the GOT slots) | `fig/session_hook.inc` |
  | `client/motion.h` | `gMotion` (the CoreMotion stand-in's hooks and stock traps), `gOrientTracker` (UI orientation, tracker, load-time snapshot) | `client/motion.inc` |
  | `providers/pump.h` | `gPump` (canvas pool, latest frame, frame count, frame rate), the CAImageQueue functions; `kSrcW`, `kSrcH`; the pump's session slots | `providers/video.inc` |
  | `providers/webcam.h` | `gWebcam` (camera, ring, frame, feed, phase lock, stats, reader file), `gWebcamPath` (backend, late renders, canvas, VideoToolbox) | `providers/webcam.inc` |
  | `providers/scene.h` | `gScene` (requested and applied source, video, scene QR, rescan), `gOrientation` | `providers/scene.inc` |
  | `providers/audio.h` | `gAudio` (source, microphone, tone, pre-roll, sessions) | `providers/audio.inc` |
  | `providers/barcodes.h` | `gBarcodes` (the metadata objects, where they were found, the Vision scan's state) | `providers/barcodes.inc` |
  | `providers/photo.h` | `gPhotoErrNext` (the next photo's test error); the still-image slots | `providers/photo.inc` |
  | `providers/mirror.h` | `gPreviewNotes` (the preview layers' angle and mirroring) | `providers/mirror.inc` |
  | `client/vision.h` | `gVisionState`, `gVisionInSwap` (the Vision pin's install state) | `client/vision.inc` |

- `profiles/`: device profiles (plists) the engine answers from.
