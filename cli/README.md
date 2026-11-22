# cli: the `stuntcam` command

Go, standard library only. `./build.sh` builds it; `go test ./...` runs the unit tests (no simulator needed).

| Command | Does |
|---|---|
| `up [--udid U] [--mode boot\|launchd] [--reboot] [--grant BUNDLE]… [--source S] [--log] [--build DIR]` | Stages the build under the host directory and arms a simulator: boot mode if it is shut down (or `--reboot`), launchd mode if it is booted. Grants camera + mic to each `--grant` bundle |
| `verify [--runtime V] [--fresh] [--keep] [--json]` | Canary verdict for this build on a runtime, on a throwaway device in the private set (`$SIMCAM_HOME/devices`, never listed by Xcode); pass/fail cached per (runtime build, engine build, stub build, canary executable, profile) in the same files and under the same per-runtime lock as `scripts/verify.sh`; inconclusive never cached, nor any run with exported `SIMCTL_CHILD_*`. A canary that crashes twice is a fail. Ctrl-C shuts the device down. Exit 0 pass, 1 fail, 2 inconclusive, 64 couldn't start |
| `down [--udid U]` | New launches stop loading stuntcam (removes the sim's `current`; launchd mode also unsets its variables, keeping other tools' entries). No reboot. Ends the sim's webcam session: the camera helper stops unless another booted sim still has `webcam` selected |
| `status [--json]` | Booted and previously armed simulators: mode (boot, launchd, booted-unarmed), build, the Xcode build recorded at arm time, the source set with `source` or `up --source` (`qr:` text shown Go-quoted; `Source` in JSON, unquoted, with any invalid UTF-8 as U+FFFD), chain, latched apps. Boot mode is read from the booted simulator's `launchd_sim` environment on the host (`BootEnv`: `stub` / `no stub`), since `launchctl getenv` in the sim can't see it |
| `swap [--udid U] [LOADER]` | Points an armed sim at another build, from the next launch |
| `chain add\|rm\|ls [--udid U] [LIB]` | Extra libraries every app launch loads (another tool's loader, an in-house debug dylib) |
| `sentinel ls\|clear [--udid U] [--bundle B]` | Crash/hang records and latches per app |
| `trace [--json] [--udid U] [--pid N] BUNDLE` | The app's last 64 camera ops, from the newest `trace-<pid>.txt` in its state dir or its container-tmp fallback (`--pid` picks another process's): op, host time, Δ ms from that photo's shutter, settings id; then one line per photo (slot 12, timestamp − slot 12, still-complete and did-finish times; incomplete, out-of-order or time-going-back photos are flagged) and the older traces. Works while the app runs (the file is at most ~150 ms behind) and after it exits. No trace: a message on stderr and exit 1 (no JSON, also with `--json`) |
| `env [--udid U]` | `export SIMCTL_CHILD_…` lines for per-launch arming |
| `source [--udid U] [--front NAME] [--back NAME] scene\|image:PATH\|video:PATH\|qr:TEXT\|webcam[:NAME]` | Switches the picture live through the simulator's control file (`$SIMCAM_HOME/sims/<udid>/control`, polled by the engine every 250 ms); apps launched later start with it. Images and videos are copied under `$SIMCAM_HOME/data/<hash>/` first, so a file in `~/Documents` works too. A simulator armed by an older stuntcam needs `up` again. `qr:TEXT`: a QR code of TEXT (see below). `webcam[:NAME]`: the Mac camera (see below) |
| `orient [--udid U] portrait\|upsideDown\|landscapeLeft\|landscapeRight\|auto` | Sets the virtual device orientation live, the same way |
| `doctor [--json]` | Xcode in use (`DEVELOPER_DIR`, else `xcode-select`) and whether `restore-rules/<its build>.json` exists and is usable; warnings for `DEVELOPER_DIR` ≠ `xcode-select -p` (`…/Xcode.app` and `…/Xcode.app/Contents/Developer` are the same), a running Xcode.app that isn't the one in use, and an armed simulator recorded for another Xcode; several installed Xcodes as information (a warning only with one of those); runtimes, host directory, build signatures, each booted simulator with its boot environment. `--json`: `{version, host, xcode, checks: [{level: ok/warn/fail/info, msg}], failed, booted_sims: [status]}` for bug reports |
| `exec [--build DIR] -- CMD…` | Runs a command (e.g. `xcodebuild test`, a CI script) so simulators it boots come up armed; for `xcodebuild test` on a launchd-armed or unarmed destination it also inserts a runner hook so UI tests' apps get the camera. Reports which apps used the camera. [Details](#stuntcam-exec) |
| `service install\|run (--only U … \| --all) [--preboot U]`, `service uninstall\|status` | Notify-only login agent: every 2 s, arms simulators that booted without stuntcam in launchd mode and posts one notification saying Xcode Run needs `stuntcam up --reboot`. `--only` limits it to the simulators named; `--all` (every simulator) has to be asked for, since other people's simulators may be booted on this Mac. It leaves alone simulators armed by stuntcam or the scripts, ones being booted armed, and ones you ran `stuntcam down` on (until their next `up`) |
| `version [--check]` | The version and commit; `--check` verifies the binary's own installed package (whatever the working directory, `--build` or `STUNTCAM_BUILD` say) against its manifest and the hashes embedded in the binary, and unpacks its payload (the Homebrew formula's test runs it) |
| `gc [--dry-run] [--keep-builds N]` | Removes private canary devices for runtimes no longer installed, state of deleted simulators, unused builds (keeps the newest 3, or N), unpacked canaries in `$SIMCAM_HOME/canary/` but the current one and the newest 2 others, `.stage-*`/`.old-*` temp dirs older than an hour, the pid files of exited camera helpers and their rings (only while the shm object at the name is still the exited helper's), and `helper/<sha>/` dirs untouched for a day that are neither this build's nor named by a running helper's pid file |

- **Webcam** (`source webcam[:NAME]`, or `up --source webcam[:NAME]` to start with it): your Mac's camera, in every
  app on that simulator, switched live like `image:`. NAME is the camera's name, a unique part of it, or its uniqueID;
  without it, the built-in camera, else the first external one (a Continuity Camera or Desk View only when named).
  `webcam:synthetic` is a moving test pattern that needs no camera. One camera feeds both the front and the back
  position (`--front`/`--back NAME` are accepted but must name the same camera). The first time, macOS asks you to
  allow camera access for "Stuntcam Camera"; `source` waits up to 60 s for the answer. The camera is on only while an
  app uses it, and turns off 10 s after. The helper quits once no booted simulator has the webcam selected (`down`,
  `source <other>`), or after 10 minutes unused. `doctor` shows its state. **Privacy:** while the webcam source is on, other programs running as you on this Mac can read the
  camera feed without their own camera permission. Other users on this Mac can't.
- **QR code** (`source qr:TEXT`, or `up --source qr:TEXT`): a QR code of TEXT, centred on white, in both camera
  positions and every orientation. TEXT is everything after `qr:`, byte for byte (scanners read it back as text, so use
  UTF-8), so quote it for the shell:
  `qr:'WIFI:S:net;T:WPA;P:secret;;'`, or `qr:$'line 1\nline 2'` for newlines. Up to 2331 bytes of UTF-8 (a QR code's
  limit at error correction M); longer text is refused. Each app makes the code itself (Core Image), so there is no
  file to stage, and the engine's barcode metadata (`AVCaptureMetadataOutput`, VisionCamera's and expo-camera's
  scanners) reports TEXT as the payload. On the way to the engine the text is percent-encoded (`%`, space, control and
  non-ASCII bytes as `%XX`); `status` shows it decoded.
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

## `stuntcam exec`

`exec [--build DIR] -- CMD…` runs a command (e.g. `xcodebuild test`, a CI script) with the boot environment exported, so
simulators it boots with `simctl boot` come up armed; a shut-down `-destination id=…` is booted armed first
(xcodebuild's own boot doesn't pass `SIMCTL_CHILD_*`). It reports which apps activated the camera and how many launches
the hook armed.

**The runner hook.** When CMD is `xcodebuild test`/`test-without-building` itself and a destination is launchd-armed or
booted unarmed, it also inserts the runner hook (`libsimcamrunnerhook.dylib`, built by `./build.sh`) into the XCTest
runner with `TEST_RUNNER_DYLD_INSERT_LIBRARIES` (a value you set is kept):
- UI tests' apps get the camera, also under ASan (exec puts the toolchain's sanitizer runtime back into the runner);
- hosted tests still pass (the hook restores `libXCTestBundleInject`) but their host app gets no camera (boot mode does
  that: `up --reboot`).

It says this before the run; for any other command it warns that test launches on such a destination get no camera.

**When the hook isn't used.**
- iOS simulators only: if any `-destination` is another platform's simulator, a device or a Mac, the hook isn't used
  (one warning); a shut-down non-iOS `id=` destination isn't booted armed.
- Also not used (one warning each) when the Xcode in use has no verified `test-runner` restore rule
  (`STUNTCAM_HOOK_FORCE=1` uses its built-in Xcode 27.0 list), when a test plan or `.xctestrun` can't be read, or when
  the runners would need different sanitizer runtimes (ASan in one test configuration and TSan or none in another: one
  variable can't give each runner its own).

**What the runner gets.** Sanitizers are read from `-enable…Sanitizer`, the `.xctestrun`'s per-target insert lists, else
the scheme's test plan (`-testPlan` or the default, every selected configuration) or its test action. Your `SIMCAM_*`
settings reach the runner as `TEST_RUNNER_SIMCAM_*`; a scheme or plan with the Main Thread Checker off keeps it off
there; Guard Malloc or a test-action `DYLD_INSERT_LIBRARIES` in the runner is replaced by the variable (said before the
run).

**A booted-unarmed destination stays prepared.** It is prepared as `stuntcam env` does and **left that way**: its
`current` and, if it had no arm record, an `env` one (`status` then says `env`). Launches there that carry stuntcam's
variables (another `exec`, `stuntcam env`) get the camera; a plain launch still doesn't. `stuntcam down --udid U`
clears it.

Measured with Xcode 27.0 only (27.0 + 26.3 runtimes; `test-without-building` and ASan through a test plan included; TSan
through the hook unmeasured).

## Command files (`cmd/stuntcam`)

One file per command or command family; `main.go` holds the command table, `usage`, `main` (dispatch and the exit
statuses) and flag parsing.

| File | What |
|---|---|
| `up.go` | `up`: refusals, the canary verdict, boot or launchd arming, the fresh control file and webcam session |
| `down.go`, `swap.go`, `env.go`, `chain.go` | `down`, `swap`, `env` (and `envArm`, which exec's runner hook also uses), `chain` |
| `verify.go` | `verify`, and the private shutdown command a verify hands its device to |
| `exec.go` | `exec`: the run directory, pre-booting destinations, runner-hook candidates, running the command, the report |
| `hook.go` | exec's runner hook: hook modes and `.env` files, `TEST_RUNNER_*` variables, `planRunnerHook`, sanitizer runtimes |
| `xcodebuild.go` | the xcodebuild in a command line (through env/arch/xcrun) and its `-destination`s |
| `status.go` | `status`, and `statusOf` (a simulator's mode, build, latches), which doctor, exec and the service use too |
| `sentinel.go`, `trace.go` | `sentinel ls`/`clear`, `trace` |
| `source.go`, `orient.go`, `control.go` | `source`, `orient`, and the simulator's control file they write |
| `webcam.go` | the Mac camera: starting its helper, ending a simulator's session, doctor's and gc's webcam parts |
| `service.go` | `service install`/`uninstall`/`status`/`run` |
| `gc.go` | `gc` |
| `doctor.go` | `doctor` and its Xcode checks |
| `version.go` | `version` |
| `sims.go`, `xcoderecord.go`, `fallback.go`, `helpers.go` | shared: picking a simulator, its boot environment, SpringBoard; `config.json`'s Xcode record; the container-tmp fallback; text helpers |
| `seams.go` | the hooks tests replace to stand in for the Mac (each defaults to the real implementation or value) |
| `golden_test.go`, `testdata/golden.txt` | the CLI's output, exit codes, simctl calls and files for a set of scenarios, run against fake tools (`go test -run TestGolden -update` rewrites it) |

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

## Package map

`cli/internal/`, bottom up (an arrow lists the stuntcam packages each one imports):

| Package | Owns | Doesn't |
|---|---|---|
| `home` | the host directory's layout (`$SIMCAM_HOME/builds`, `sims/<udid>/…`, `verdicts`, …), the `current` file, atomic writes | import any other stuntcam package |
| `simctl` | every `xcrun simctl` call (the only package that runs it), the private device set, stopping calls in flight on a signal | know about arming or verdicts |
| `xcode` | which Xcode is in use (`DEVELOPER_DIR`, `xcode-select`, `xcodebuild -version`) and its restore-rules file | touch a simulator |
| `procenv` | a booted sim's boot environment, read on the host from its `launchd_sim` process (`KERN_PROCARGS2`) | run anything inside the simulator |
| `scheme` | Xcode schemes, test plans and `.xctestrun` files: inserted libraries, sanitizers, test configurations | change any of them |
| `trace` | parsing `trace-<pid>.txt` and the `stuntcam trace` report (JSON schema 1, the table) | find the simulator or the app (the caller passes the files) |
| `artifacts` → home | finding a build (`--build`, a checkout, the installed package), checking the installed package against its manifest, unpacking and staging files into content-hash dirs | arm or boot anything |
| `state` → home | the engine's per-app events, latch markers and container-tmp fallback (`sentinel`), with the engine's latch rules | write anything but its clear records and removals |
| `arm` → home, artifacts, xcode | a sim's arming state (`current`, `config.json`, `arm.json`, the down marker) and its boot or launchd environment, through the `Sim` interface | call simctl itself (it gets a `Sim`) |
| `service` → home, simctl | the login agent's decisions (which booted sims to arm), its plist and its notification | arm (cmd/stuntcam does, with `arm`) |
| `webcam` → home, procenv | the `webcam` source's CLI side: cameras, the camera helper's start, stop and pid files, which sims use it, its gc (docs/webcam-ring.md) | call simctl |
| `verify` → home, simctl, artifacts, arm | the canary self-test: verdict cache, per-runtime lock, private device, boot, launch, the detached shutdown | boot, shut down or arm any device outside the private set |

### `cli/cmd/stuntcam`

The `stuntcam` binary: argument parsing, one function per command, and what the commands print. Its files are
listed under [Command files](#command-files-cmdstuntcam) above.
