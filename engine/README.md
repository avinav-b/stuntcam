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
  (`libXCTestBundleInject`, MTC, RPAC: restore-rules `test-runner`). Inert on boot-armed sims (design §4.2).
- `src/simcam.m`: the engine's single translation unit. It includes the fragments below **in order**: they share
  file-scope state and `__attribute__((constructor))` functions run in source order.
  - `fig/`: answers to the simulator's capture layer: capture sources and their attributes/formats, per-session
    state, session adoption and teardown.
  - `core/`: runtime-shape checks and the preflight gate, the single install gate and constructors, the host control
    file and per-launch env, the pause-aware op watchdog and crash sentinel (`safety.inc`), and lazy arming (`lazy.inc`, last): traps on public capture entry points open the gate
    on first use.
  - `providers/`: producers: frame source and scene, preview, barcode fixture, audio, the frame pump and movie
    sinks, photo encoding.
  - `client/`: small stock-first fixes on public classes where the simulator build compiles a path out (CoreMotion,
    photo data, rotation coordinator, output settings, Vision barcode revision). Each has an off switch.
- `profiles/`: device profiles (plists) the engine answers from.
