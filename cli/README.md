# cli: the `stuntcam` command

Go, standard library only. `./build.sh` builds it; `go test ./...` runs the unit tests (no simulator needed).

| Command | Does |
|---|---|
| `up [--udid U] [--mode boot\|launchd] [--reboot] [--grant BUNDLE]… [--source S] [--log] [--build DIR]` | Stages the build under the host directory and arms a simulator: boot mode if it is shut down (or `--reboot`), launchd mode if it is booted. Grants camera + mic to each `--grant` bundle |
| `verify [--runtime V] [--fresh] [--keep] [--json]` | Canary verdict for this build on a runtime, on a throwaway device in the private set (`$SIMCAM_HOME/devices`, never listed by Xcode); pass/fail cached per (runtime build, engine build, stub build, canary executable, profile) in the same files and under the same per-runtime lock as `scripts/verify.sh`; inconclusive never cached, nor any run with exported `SIMCTL_CHILD_*`. A canary that crashes twice is a fail. Ctrl-C shuts the device down. Exit 0 pass, 1 fail, 2 inconclusive, 64 couldn't start |
| `down [--udid U]` | New launches stop loading stuntcam (removes the sim's `current`; launchd mode also unsets its variables, keeping other tools' entries). No reboot |
| `status [--json]` | Booted and previously armed simulators: mode (boot, launchd, booted-unarmed), build, the Xcode build recorded at arm time, chain, latched apps. Boot mode is read from the booted simulator's `launchd_sim` environment on the host (`BootEnv`: `stub` / `no stub`), since `launchctl getenv` in the sim can't see it |
| `swap [--udid U] [LOADER]` | Points an armed sim at another build, from the next launch |
| `chain add\|rm\|ls [--udid U] [LIB]` | Extra libraries every app launch loads (another tool's loader, an in-house debug dylib) |
| `sentinel ls\|clear [--udid U] [--bundle B]` | Crash/hang records and latches per app |
| `trace [--json] [--udid U] [--pid N] BUNDLE` | The app's last 64 camera ops, from the newest `trace-<pid>.txt` in its state dir or its container-tmp fallback (`--pid` picks another process's): op, host time, Δ ms from that photo's shutter, settings id; then one line per photo (slot 12, timestamp − slot 12, still-complete and did-finish times; incomplete, out-of-order or time-going-back photos are flagged) and the older traces. Works while the app runs (the file is at most ~150 ms behind) and after it exits. No trace: a message on stderr and exit 1 (no JSON, also with `--json`) |
| `env [--udid U]` | `export SIMCTL_CHILD_…` lines for per-launch arming |
| `source [--udid U] scene\|image:PATH\|video:PATH` | Switches the picture live through the simulator's control file (`$SIMCAM_HOME/sims/<udid>/control`, polled by the engine every 250 ms); apps launched later start with it. Images and videos are copied under `$SIMCAM_HOME/data/<hash>/` first, so a file in `~/Documents` works too. A simulator armed by an older stuntcam needs `up` again |
| `orient [--udid U] portrait\|upsideDown\|landscapeLeft\|landscapeRight\|auto` | Sets the virtual device orientation live, the same way |
| `doctor [--json]` | Xcode in use (`DEVELOPER_DIR`, else `xcode-select`) and whether `restore-rules/<its build>.json` exists and is usable; warnings for `DEVELOPER_DIR` ≠ `xcode-select -p` (`…/Xcode.app` and `…/Xcode.app/Contents/Developer` are the same), a running Xcode.app that isn't the one in use, and an armed simulator recorded for another Xcode; several installed Xcodes as information (a warning only with one of those); runtimes, host directory, build signatures, each booted simulator with its boot environment. `--json`: `{version, host, xcode, checks: [{level: ok/warn/fail/info, msg}], failed, booted_sims: [status]}` for bug reports |
| `exec [--build DIR] -- CMD…` | Runs a command (e.g. `xcodebuild test`, a CI script) with the boot environment exported, so simulators it boots with `simctl boot` come up armed; a shut-down `-destination id=…` is booted armed first (xcodebuild's own boot doesn't pass `SIMCTL_CHILD_*`). When CMD is `xcodebuild test`/`test-without-building` itself and a destination is launchd-armed or booted unarmed, it also inserts the runner hook (`libsimcamrunnerhook.dylib`, built by `./build.sh`) into the XCTest runner with `TEST_RUNNER_DYLD_INSERT_LIBRARIES` (a value you set is kept): UI tests' apps get the camera, also under ASan (exec puts the toolchain's sanitizer runtime back into the runner), hosted tests still pass (the hook restores `libXCTestBundleInject`) but their host app gets no camera (boot mode does that: `up --reboot`). It says this before the run; for any other command it warns that test launches on such a destination get no camera. iOS simulators only: if any `-destination` is another platform's simulator, a device or a Mac, the hook isn't used (one warning); a shut-down non-iOS `id=` destination isn't booted armed. Also not used (one warning each) when the Xcode in use has no verified `test-runner` restore rule (`STUNTCAM_HOOK_FORCE=1` uses its built-in Xcode 27.0 list), when a test plan or `.xctestrun` can't be read, or when the runners would need different sanitizer runtimes (ASan in one test configuration and TSan or none in another: one variable can't give each runner its own). Sanitizers are read from `-enable…Sanitizer`, the `.xctestrun`'s per-target insert lists, else the scheme's test plan (`-testPlan` or the default, every selected configuration) or its test action. Your `SIMCAM_*` settings reach the runner as `TEST_RUNNER_SIMCAM_*`; a scheme or plan with the Main Thread Checker off keeps it off there; Guard Malloc or a test-action `DYLD_INSERT_LIBRARIES` in the runner is replaced by the variable (said before the run). A booted-unarmed destination is prepared as `stuntcam env` does and **left that way**: its `current` and, if it had no arm record, an `env` one (`status` then says `env`). Launches there that carry stuntcam's variables (another `exec`, `stuntcam env`) get the camera; a plain launch still doesn't. `stuntcam down --udid U` clears it. Measured with Xcode 27.0 only (27.0 + 26.3 runtimes; `test-without-building` and ASan through a test plan included; TSan through the hook unmeasured). Reports which apps activated the camera and how many launches the hook armed |
| `service install\|run (--only U … \| --all) [--preboot U]`, `service uninstall\|status` | Notify-only login agent: every 2 s, arms simulators that booted without stuntcam in launchd mode and posts one notification saying Xcode Run needs `stuntcam up --reboot`. `--only` limits it to the simulators named; `--all` (every simulator) has to be asked for, since other people's simulators may be booted on the Mac. It leaves alone simulators armed by stuntcam or the scripts, ones being booted armed, and ones you ran `stuntcam down` on (until their next `up`) |
| `version [--check]` | The version and commit; `--check` verifies the binary's own installed package (whatever the working directory, `--build` or `STUNTCAM_BUILD` say) against its manifest and the hashes embedded in the binary, and unpacks its payload (the Homebrew formula's test runs it) |
| `gc [--dry-run]` | Removes private canary devices for runtimes no longer installed, state of deleted simulators, unused builds (keeps the newest 3), unpacked canaries in `$SIMCAM_HOME/canary/` but the current one and the newest 2 others, and `.stage-*`/`.old-*` temp dirs older than an hour |

- **Which simulator:** `--udid`, else the only booted one, else the most recently booted. `up` says which.
- **Which build:** the installed package's own files. `--build DIR` (or `STUNTCAM_BUILD`) uses another build; a stuntcam
  checkout in the working directory is used only with `STUNTCAM_USE_CHECKOUT=1`.
- **Self-test:** before arming, `up` runs the canary on a throwaway simulator (cached per runtime). A failed self-test
  means no arming; `--allow-unverified` overrides.
- **Integrity:** every packaged file is checked against a manifest whose hash is built into the binary. A changed, added
  or missing file is an error. `stuntcam version --check` runs just that check.
- **Xcode versions:** `up` records the Xcode in use. For an Xcode build without restore rules, apps print
  `restore rules unverified for Xcode <build>`. After switching Xcode, run `stuntcam up` again.
- **Exit codes:** 0 ok, 1 failed, 2 inconclusive (`verify`), 64 wrong arguments. `exec` passes the command's own code
  through. `-h` after a command lists its options.

## `stuntcam trace --json` (schema 1)

Fields are only ever added within schema 1, never renamed or removed.

| Field | Meaning |
|---|---|
| `schema` | `1` |
| `udid`, `simulator`, `bundle`, `pid`, `build` | the simulator, the app, its process, the engine build that wrote the file |
| `running` | `true` if that pid is the app's running process on the simulator (`launchctl list` in the sim), `false` if not, `null` if it couldn't be asked |
| `file`, `location` | the file read; `"state-dir"` or `"container-tmp"` |
| `written`, `written_ns` | when the app last wrote it (RFC 3339 UTC, ns; CLOCK_REALTIME ns) |
| `recorded`, `ring_size` | ops recorded since the process started; the ring keeps the last `ring_size` (64) |
| `timebase` | `{numer, denom}`: mach ticks × numer / denom = ns |
| `ops[]` | oldest first: `index` (op number in the process), `op`, `wall` / `wall_ns` (CLOCK_REALTIME), `mach` (mach_absolute_time), `host_ns` (the host-time clock `AVCapturePhoto.timestamp` uses), `settings_id` (`null` for ops without one), `arg`, `delta_ms` (photo ops: from that photo's `shutter`, else its `slot12`; else `null`) |
| `photos[]` | per settings id, in ring order: `stages` (op names), `in_order` (stages in the order below, each once), `monotonic` (host times never go back), `complete` (shutter, slot12, still-complete and, unless it failed, timestamp), `error` (still-complete's status; 0 = delivered), `host_ns` (stage → host ns), `ms` (stage → ms from the shutter), `timestamp_minus_slot12_ms`, `timestamp_s` (the photo's timestamp, seconds) |
| `others[]` | the bundle's other traces, newest first: `pid`, `file`, `location`, `written` |

Ops: `activate`; `session-start` (arg: configuration id), `session-stop`; per photo, in order: `shutter`
(`capturePhotoWithSettings:delegate:`), `slot12` (Fig slot 12, `IrisStillImageSinkCaptureImage`), `will-capture`, `timestamp`
(arg: the photo's timestamp in host ns; taken with its source frame), `did-capture`, `encode-start`, `encoded` (arg: bytes),
`still-complete` (arg: error status), `did-finish` (AVF ran `didFinishCaptureForResolvedSettings:`); `prepare` (slot 13, the
prepared settings' id); `movie-start`, `movie-stop` (arg: status); `latch-<feature>`; `crash` (arg: signal). `SIMCAM_TRACE=0` in
the app's environment turns the ring off.
