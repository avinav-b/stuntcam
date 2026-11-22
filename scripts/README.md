# scripts/

The test scripts: zsh scripts that build the test apps, arm simulators, launch apps and run the test suites on a Mac with
Xcode. Each script's header says how to call it and what it prints; `--help` prints that header where a script has it.

Most of them need:
- `./build.sh` (the engine, stub and loader) and the test apps (`scripts/build-fixtures.sh`);
- `SIMCAM_UDID_<rt>` set to the UDID of your simulator for runtime `<rt>` (`263` = iOS 26.3, `27` = 27.0, `175` = 17.5);
- a sim armed with `scripts/boot.sh <rt> stub` ("armed" below), unless the line says otherwise.

Results go to `$SIMCAM_RESULTS` (`$SIMCAM_HOME/results`; `<clone>/results` links there).

## Historical names

Some bundle ids, product names, inputs and log prefixes keep short historical names (m5, m10, m12, ...). Apps, logs
and saved results already use them, so they stay as they are:
- `dev.simcam.spike.m10canary` / `m10canary.app`: the canary (`canary/`).
- `dev.simcam.spike.m10plain` / `m5app.app` (process `m5app`): the plain app (`fixtures/plain`). Its inputs are
  `M5APP_*`, its log lines start `M5APP| `, and its golden mode's inputs and log tokens are `GF_*` and `GFH`,
  `GFSTAT`, ... (golden frames).
- `dev.simcam.spike.m12plain` / `p12.app` (process `p12`, "plain12"): plain-orient (`fixtures/plain-orient`), run by
  `p12.sh`. Its inputs are `P12_*` and its log lines start `P12| `.
- `dev.spike.m1bcam` / `m1bcam.app`: the React Native app (`fixtures/expo`, VisionCamera and expo-camera). Its log is
  `m1b.log` and its lines start `M1B`. `dev.spike.m7cam`: the Flutter app (`fixtures/flutter`), with `M7_*` inputs,
  `m7.log` and `M7| ` lines.
- `cfg-m10.plist`: the camera profile the package ships.
- `mutation-L<nn>` (activation.sh cases) and the id column of `mutation.sh`: `L<nn>` is a regression id, one per
  fixed bug. The case turns that fix's `SIMCAM_OFF_*` switch on and passes if the test catches the bug.

## Sourced by the others

- `env.sh` — `$SIMCAM_HOME` and its layout, staging of profiles, media and restore rules (simulator apps can't read a
  clone under ~/Documents); sourced first by every script that touches a simulator or a build (not by
  `build-fixtures.sh`, `package.sh` and `metro.sh`).
- `common.sh` — shared helpers: `print_usage` (`--help`), `need_udid`, `inject_env` (per-launch injection),
  `wait_for_line`.
- `arm-env.sh` — the one list of variables that arms a sim, for `boot.sh`, `launchd-arm.sh`, `swap.sh` and `verify.sh`.
- `fenv.sh` — a local Flutter setup (`FLUTTER_SDK`, caches under `build/flutter/`), for `build-fixtures.sh flutter`.
- `lc-hook.sh`, `src-hook.sh` — `HOOK=` files for `p12.sh`: lifecycle rounds (background, second camera app,
  interrupts) and live source switches through the control file.

## Build

- `build-fixtures.sh` — builds the test apps (canary, plain, plain-orient, expo, flutter) and installs them on the
  `SIMCAM_UDID_*` sims. Needs Xcode, CocoaPods and npm (expo), `FLUTTER_SDK` (flutter).
- `package.sh` — builds the npm/Homebrew package and the GitHub action into `build/package/`; publishes nothing. Used by
  `.github/workflows/release.yml`.
- `ci-verdicts.sh` — after `package.sh`: runs the packaged canary once per general iOS runtime installed here and
  packages again with the plain passes as shipped verdicts (a confirmed fail fails it). Needs a scratch `SIMCAM_HOME`.
  Used by `.github/workflows/release.yml`. `ci-verdicts-test.sh` tests its branches against stubs (bash, zsh, node; no
  simulator).
- `metro.sh` — starts or stops Metro for `fixtures/expo` (`$EXPO_PORT`, default 8099), for the `vc*`/`expo*` apps.

## Arm a simulator

- `boot.sh` — (re)boots a sim armed at boot (`stub`), armed in launchd mode (`launchd`) or plain (`plain`).
- `launchd-arm.sh` — arms or disarms an already-booted sim in launchd mode, no reboot.
- `swap.sh` — points an armed sim at another engine build without a reboot.

## Run one app

- `run.sh` — one launch of a test app (canary, plain, vc*, expo*, flutter*) on an armed or plain sim; writes
  `results/<app>-<rt>-<inj>-<tag>/` and prints one summary line. Most suites below drive it.
- `lib.sh` — one launch of the RN (`vcorient`, `expoorient`) or Flutter fixture with per-launch injection or `NOINJ=1`.
- `p12.sh` — one launch of `fixtures/plain-orient` in a mode, camera and orientation; `HOOK=` runs a script each poll.
- `cycle.sh` — one launch of the RN fixture's open/close cycle mode (also under a sanitizer build).
- `webcam-app.sh` — one run of `fixtures/plain` with the webcam source, with the app's CPU use.

## Suites

- `verify.sh` — the canary verdict on a throwaway sim in a private device set, cached per build (the CLI's
  `stuntcam verify` follows the same key, lock and cache format). Needs `./build.sh` and the canary; no armed sim and
  no `SIMCAM_UDID_*`.
- `conformance.sh` — the safety and arming paths: kill switch, motion gate, broken `current`, proto skew, photo latch,
  crash latch.
- `activation.sh` — bounded lazy activation, sentinel latches, crash attribution, CoreMotion's load-time decision, the op
  trace. The cases are in `activation/`, one file per concern. Needs `cli/bin/stuntcam`
  for the trace cases.
- `mutation.sh` — turns each fix's off switch on in turn and checks that the canary catches it as each mutant's
  row expects (catch, or a known miss that another test guards).
- `bgfg.sh` — the engine follows the app's start/stop calls across background and foreground; the per-run check is
  `bgfg-check.py`. Needs `fixtures/bgfg-probe`.
- `features.sh` — camera features stuntcam can't deliver read as unsupported. Needs
  `fixtures/features-probe`.
- `wd-traced.sh` — the watchdog doesn't count a hang while a debugger is attached; attaches lldb with `lldb-attach.py`.
- `memcycle.sh` — memory stays flat over open/close cycles in one process.
- `golden.sh` — captures and compares the hashes of every frame the scene, image and video sources deliver
  (docs/ARCHITECTURE.md).
- `webcam-test.sh` — the webcam source end to end, with the camera helper (`helper/dev.sh`), on the 263 and 27 sims.
- `qr-test.sh` — the `qr:` source end to end on a sim armed with `stuntcam up`: each text case through
  `stuntcam source qr:`, every scanner of the plain, VisionCamera, expo and Flutter apps (`qr-check.py` compares the
  exact text); the plain app in both camera positions and every orientation, the others back/portrait plus one
  front/landscape run; and a live switch's latency.
- `torture.sh` — many launches of the RN cycle modes through `cycle.sh`, optionally ending with `memcycle.sh`.
- `matrix.sh` — every app once on one runtime through `run.sh`.
- `loop.sh` — n launches cycling through the given apps (a soak); `final.sh` runs the vcfull loop and the 50-launch soak.
- `orient-lib.sh`, `orient-plain.sh` — preview, photo and movie orientation for back/front x portrait/landscape (the RN
  or Flutter fixture, and plain-orient); `reclass.sh` re-classifies existing `lib-*` results. They need
  `tools/bin/upright` and `tools/orient.py` (Python with Pillow).

## Debugging

- `lldb-attach.py` — attaches lldb from the command line to one of our sim apps and keeps it traced until a log line
  appears (used by `activation.sh` act-traced and `wd-traced.sh`). Run with Xcode's python3 and
  `PYTHONPATH=$(xcrun lldb -P)`.
