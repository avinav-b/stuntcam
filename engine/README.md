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
  | `fig/sources.inc` | the capture sources the simulator's capture layer asks for: the object ABI, the source model, attributes, formats, the source factories, the microphone source |
  | `core/act.inc` | activation's shared state: the state machine's states and phases, test-fault parsing, the hook table (resolve in PREPARE, install in SWAP) |
  | `core/preflight.inc` | runtime detection, the session vtable layout check, the preflight gate (all or nothing); also the Mach-O load-command walk the crash handler and the session hook share |
  | `core/safety.inc` | the op watchdog and crash sentinel: overview, shared state, the per-thread feature key, the signal-safe event writer, ops |
  | `core/crash_handler.inc` | the sentinel's signal handlers and the image ranges a crash is attributed against |
  | `core/markers.inc` | the sentinel's load-time state and its marker files (`latched-<build>`, `preflight-fail-<build>`…): paths, writes, prunes |
  | `core/events_file.inc` | the events file's writers outside the crash handler: trim, locked append, the record queue |
  | `core/sentinel_activate.inc` | the sentinel's activation steps: PREPARE 1 (read the past, decide latches) and START 1-3 |
  | `core/trace.inc` | the op trace ring that `stuntcam trace` reads |
  | `core/init.inc` | process start, one engine per process, the log, PREPARE 2 (lookups), the source factories' resolve step |
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
  | `core/lazy.inc` | lazy arming, last: traps on the public capture entry points open the gate on the first camera call (PREPARE, SWAP, START) |
- `profiles/`: device profiles (plists) the engine answers from.
