# stuntcam: design v4.1 (2026-11-11)

Tags: **[M x]** measured in spike file x (raw results and spike reports are the maintainer's working notes, not published). **[R x]** read in source or in research file x (internal research, not published). **[S]** designed or speculative, not run.

Setup: one Apple Silicon Mac, Xcode 27.0, iPhone 17 Pro sim. Runtimes **17.5** (21F79), **26.3** (23D8133) and **27.0** (24A434). **When this was written no GitHub-hosted runner had run this code**, so its CI claims are [R] or [S]; W1 (U7) later ran the engine and its self-test on `macos-26` runners (MATRIX.md).

**v4 → v4.1.** Round 4 scored v4 at 100/100/100 (three internal evaluations), and all three evaluators said "ready to start v1.0". v4.1 applies every round-4 must-fix (§2) and corrects v4's evidence overclaims. It also adds a week-1 checklist (§10) and a reworked uncertainties table (§11). Nothing new was measured, so every addition is [S] until W1-W4 run it.

## 1. Summary, name, licence
**What it is.** stuntcam gives the iOS Simulator a camera with no app change. It answers at the private **Fig layer** under AVFoundation. That means discovery, devices, formats, inputs, outputs, connections, KVO, notifications and error paths all run on **Apple's own AVFCapture objects and code**. The default source is an in-sim synthetic scene carrying a QR code and an EAN-13 barcode, so no helper process is needed. Webcam input comes in v1.1, and the README's first screen says so.

**Name `stuntcam`.** Free on npm, Homebrew and PyPI; the repo is `avinav-b/stuntcam`, the npm package is the unscoped `stuntcam` under the maintainer's account, and a trademark check is due before launch [R internal research §5]. **Licence Apache-2.0**: a patent grant, and inbound code fits (serve-sim and baguette are Apache, idb is MIT). Every file carries an SPDX header, and `THIRD_PARTY_NOTICES.md` lists the URL and SHA of every borrowed file [R §6]. **Simulator only:** the dylib is platform-7 (simulator only), so release and device builds are untouched. Media carry `Software = stuntcam <ver>` [S].

**Why this beats the 20+ existing tools.** Every one of them fakes AVFoundation's client objects: serve-sim, SimulatorCamera, baguette, idb VideoInjection, SimCam, RocketSim and the long tail [R internal research §1.0]. Their issue trackers are dominated by those fakes crashing when Apple code reads private state: serve-sim #169/#183/#190, baguette #121/#132, SimCam #4/#6, RocketSim #838, and idb's "Rules for the shim" [R §1.1 A]. On our fixtures: serve-sim offered one 720p format and failed QR, torch and focus [M m1a, m1b]; SimulatorCamera offered 0 formats, and VC5 could not mount on it [M m1a, m1b].

stuntcam has no fake client objects, so that crash class is absent by construction. Measured so far:
- **Coverage:** one **engine** build passes plain AVF, VC5, expo-camera, Flutter `camera` + `mobile_scanner` and a full-check mode (VisionCamera 5.2 with a patch, 8-camera profile) on 17.5, 26.3 and 27.0. Full-check mode is 33/34, and the 34th check is 4K60 recording, a known cap [M m10]. Arming was measured separately [M m11]; the W1 exit test joins the two (§9).
- **Stability:** 210 boot-armed launches with 0 crash reports [M m10]; 200 teardown cycles clean under TSan and ASan [M m12].
- **Formats:** 10 formats from 720p to 4K at 30/60, and photos at 4032×3024 [M m5, m8].
- **Porting cost:** the 26→27 port was +34/−9 LOC and 1 key, and the unported build failed safe [M m9].

**Xcode Run.** It needs the loader in place before the sim boots. That is parity with SimCam (#14 [R internal research §1.1]), not a weakness unique to stuntcam. The public matrix will show competitor columns at pinned SHAs [S].

## 2. Round-4 must-fixes → resolution (A/B/C = the three round-4 evaluations)
The round-3 must-fixes were resolved in design v4 §2 (internal), and all three round-4 evaluators accepted the boot-default rebuttal there.

| # | Must-fix | Resolution in v4.1 (evidence / where) |
|---|---|---|
| A-MF1, B-MF8 | One tree: m11 arming + m10/m12 engine; W1 exit test | **W1 exit criterion** (§10). Until it passes, claims read "engine build passes; arming measured separately" |
| A-MF2 | Public-class hooks must not wait for the first camera call | CoreMotion hooks install in the dylib constructor (public class, no private call). The fake mic gets its own trampoline on the audio-source factory, which also triggers activation. Three ordering fixtures × 3 runtimes, plus a two-manager motion fixture (§4.1, U2) |
| A-MF3 | Evidence overclaims | Corrected throughout (§3 note; §6; §8). 36/36 means 18 per runtime. Mutation suite: 26.3, pre-merge only. Ledger is [S]. TSan, lifecycle and soak runtimes are unrecorded. T3-via-chain is [S]. 34th check named. EXIF in-pipeline = DTO + Orientation 6 only. `setOutputSettings:`, `qr:TEXT`, tone A/V offset and Xcode Run on the merged build are unmeasured. 110 constants are gone in 27 |
| A-MF4 | Mic TCC with app-owned sessions | W1 test, 10 min (U9). Until it passes, "no TCC in CI" is [S] |
| B-MF1 | Pause-aware timeouts | Timeouts never latch while traced. A tick-gap detector restarts deadlines after a debugger stop, suspension or sleep. A hang latch needs 2 hangs or a sample with our frames. Ops end before app delegates run. The stop barrier excludes encoder drain. Sanitizer runs use ×10 timeouts (§5.4, U5) |
| B-MF2 | Sentinel: steady-state crashes, self-resolving, decay | Provider-thread/PC attribution with no marker. In-process async-signal-safe attribution, so no CLI is needed. Latches decay after 20 activations or 24 h (§5.4) |
| B-MF3 | Fingerprint inheritance; canary tri-state | Code hashes are out of the fp. An unknown fp with a green gate inherits the nearest same-major entry, and the canary decides. Verdicts are `pass` / `fail(check)` / `inconclusive(reason)` ("couldn't run", never cached) (§5.2) |
| B-MF4 | Frozen stub + swappable loader + singleton + proto | The boot env names only a frozen stub and `sims/<udid>/`. The stub reads `current` → `loader.dylib`, so upgrades need no reboot. Second engines go inert. Every host file carries `proto` (§4.1, U14) |
| B-MF5 | Restore rules keyed to the launching Xcode | The Xcode build recorded at arm time (`sims/<udid>/config.json`; ⌘R names no Xcode in-process, e26) selects `restore-rules/<xcode-build>.json`, which the loader reads. Unknown Xcode → newest rules + one line. `doctor` flags Xcode mismatches. Insert-list diff in `tools/inventory` still TODO (§4.1, U4) |
| B-MF6, A-S7 | Canary before arming; boot-health fallback; private device set | `up` order: verdict → arm. Canary devices live in `simctl --set <root>/devices`, so Xcode never lists them. A failed armed boot falls back to unarmed + launchd + reason. `gc` prunes canary devices (§4.5, §5.3) |
| B-MF7 | MTC-clean release gate | Matrix with MTC loaded and `MTC_CRASH_ON_REPORT=1` gives 0 reports in our frames (§8). Orientation is read off-UIKit (§6.4 rule 2) |
| C-MF1, B-S | Prebuilt dylibs, canary app, verdicts; first-run time | Release ships linker-signed universal dylibs, a prebuilt canary `.app` and verdicts for gate-tested runtime builds (copied linker-signed dylibs load [M m4 Q4]). The verdict key uses the manifest's dylib sha. `--from-source` builds locally. First-ever canary is timed (U1) |
| C-MF2, B-A8 | Xcode-booted silent miss | README Quick Start: **`stuntcam up` before ⌘R**, and again after a Mac reboot, Simulator quit or Erase. `up` prints "armed until this sim shuts down". **Notify-only `stuntcam service` moves into v1.0**: it launchd-arms new boots and posts a notification, with opt-in `--preboot <udid>` at login and no auto-reboot. U17 logs re-arm frequency (§4.6) |
| C-MF3 | Movie `maxRecordedDuration`/`maxRecordedFileSize` | **In v1.0, W4.** Stop with the AVF error + `AVErrorRecordingSuccessfullyFinishedKey=YES` [R internal compat map :255, :2037]. Canary assertion + ledger entry. Pause/resume stays v1.1, with a doctor line (§6.1) |
| C-MF4 | Production-app residuals | `stuntcam trace` added (§4.5). First shutter ≥5 s after mount. Two-manager CoreMotion pre-check (U2). 34th check = 4K60, expected (§9) |

**Should-fixes taken:**

| Item | Where |
|---|---|
| Xcode 26.6/27.1/27.2 in scope (A-S1) | U4 |
| Scan `.xcscheme` for `DYLD_INSERT_LIBRARIES` and offer `chain add` (A-S2) | `up` |
| Chain loss window stated; `status`/`doctor` report drift but never write (A-S3, B-A5) | — |
| Watermark checks for VDO, full-check mode and 17.5 (A-S4) | — |
| U11 auto-arm probe | W1 (A-S5) |
| Flutter 27.0 landscape marked untested (A-S8) | — |
| Default chain scope = apps + extensions (B-A6) | — |
| Latched-start rate limit (B-B3) | — |
| Concurrent-activation torture (B-A10) | U13 |
| Preflight cache excludes the profile round-trip (B-B11) | — |
| Production-app acceptance (§9) run once in boot mode with the T3 loader chained (B) | — |
| Debug assertion "AVF call from provider" (B) | — |
| Timed human port 26→27 on the split tree | W6 (B) |
| Support policy + `doctor --json` issue template (B) | — |
| Sentinel burn-down rule (C) | — |
| Engine C API raised with serve-sim | W6 (C) |
| Private scratch repo for runners | offered to the user (C) |

**Decided, not taken:** MCP stays in v1.1, so O10 stays 3; W6 has no slack left after the service and movie limits. `exec` doesn't rewrite `xcodebuild` args; parallel-testing clones are documented instead (U19).

## 3. Architecture
```
 App + any stack (AVCam, VC5, expo-camera, Flutter camera, mobile_scanner)               UNCHANGED
   │ public AVFoundation          + client hooks (stock-first; CoreMotion eager, rest lazy; off switches)
 Genuine AVFCapture: session, devices, formats, inputs, outputs, connections, preview layer   APPLE CODE
   │ Fig source + session vtables · CM notification centre · Fig local queues                APPLE ABI
 libstuntcam.dylib (engine; singleton; apps only): sources from profile │ session slots │ notify │ providers
   │ encoders: ImageIO / AVAssetWriter in-sim                                               APPLE CODE
 loader.dylib (swappable, no reboot): Xcode-keyed restore rules · chain · engine dlopen · proto check
 stub (frozen, ≤62 lines, libSystem only): process gate → read sims/<udid>/current → dlopen loader
 host: CLI (up/exec/status/doctor/trace/source/verify/service) · ~/Library/Application Support/stuntcam
```
**Who owns what.** Apple's code is every object the app touches, plus validation, KVO, notification fan-out, error mapping, point conversions and encoders. Ours is what the sim stubs out: Fig sources answering from a profile, session slots for configure/start/stop/photo/movie, producers, and a few client patches where sim AVF compiles paths out.

**Prototype size [M m10].** ~1,800-line engine, 62-line stub, ~600 lines of test apps.

**Private dependencies by category** (full per-runtime inventory: internal notes from spikes m2, m5 and m9; regenerated by `tools/inventory`):

| Category | Count | Variance seen |
|---|---|---|
| ObjC entry points (video-source, audio-source, session factories) + 1 ivar | 3 + 1 | 17.5: one method-signature difference, handled [M m10] |
| Called, not hooked: 27's source-manager method, Apple's Attributes initializer | 2 | new in 27 [M m9] |
| C functions (CoreMedia/CMCapture) | 9 | 27 dropped the source-list function [M m9] |
| Vtables: source (2 classes, 3 slots); session (8 replaced + base Invalidate/Finalize) | 2 | session grew 19→24 in 27; replaced slots kept their positions [M m9] |
| Contract: 1 local-queue message type; ~25 notification names + keys | ~26 | 3,516 constants unchanged; **110 attribute constants removed** in 27 [M m9] |
| Data schemas: attributes, format keys, barcode capability/payload | 3 | 27 typed the attributes; Apple's initializer absorbs it [M m9] |
| Semantic switches (rotation encoding, mic, Vision rev, interruption map …) | ~7 | 17.5 rotation is an orientation value [M m6] |
| Loader: sim insert, boot env, signature rules, Xcode's 4 libs, XCTest inject list | 5 | Xcode 27.0 only [M m4, m6, m11] |

**Client hooks.** v3 counted 18 selectors in 7 areas: photo data, RotationCoordinator, preview contents, accel-only CoreMotion, Vision rev-1 pin, writer settings, photo-finished. m10 replaced metadata types with a profile fix; m12 added preview mirroring, the rotation formula and interruptions. The exact count is a W1 `tools/inventory` output that the gate consumes. Hooks are **stock-first** (call the original, override only the sim's broken answer), and each has a `STUNTCAM_FIX_<name>=0` off switch.

## 4. Arming
### 4.1 Boot mode (default when `up` boots the sim)
`SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=<stub> simctl boot` reaches every sim process and beats Xcode, per-launch, launchctl and UI-test values [M m1a 1d, m4 Q2].

**Frozen stub + swappable loader** (B-MF4) [S; m11's chain proved a later-added lib loads on the next launch, M m11 §1e]: (1) The boot env names only the stub and `sims/<udid>/`. (2) On each eligible launch, the stub (libSystem only, never changed after 1.0) reads `current` and dlopens `builds/<sha>/loader.dylib`. The loader holds the restore rules and the chain, and dlopens the engine. (3) Upgrades, rule fixes and new Xcode lib lists take effect at the next launch, with no reboot. (4) Every config, control, status and verdict file carries `proto`. A newer major `proto` gives no camera plus one line. (5) The engine is a process singleton: the first claims `stuntcam_engine_v1`, and a later copy (other build or mode) logs "duplicate ignored" and goes inert. (6) Chain entries whose basename is stuntcam's are dropped.

**Restore rules [M m11], keyed to the launching Xcode** (B-MF5):
1. **Hosted tests** (`XCTestBundleInjectPath`): load exactly that list. ⌘U and `xcodebuild test` pass in ~9 s; without the rule they hang. **Except launch-only libraries**, in this and every rule and the chain: `libclang_rt.*` (sanitizer runtimes) and `libgmalloc` are never dlopened [M e26, e28]. A sanitized app links its own runtime; the list's copy (from the test bundle) aborts ASan when dlopened ("Interceptors are not working") and maps a second TSan. Nor is a second copy of any library whose basename is already mapped. The loader logs one line, `skipped (must load at launch, or a second copy): <path> [already loaded: <path> | not in this process | second copy; already loaded: <path>]`; a chain entry skipped as a second copy is also said on stderr, since the chain is the user's own list [e30].
2. **UI-test target** (`XCODE_SCHEME_NAME`): MTC + RPAC, plus log redirect when `IDE_CHOSEN_LOGGING_METHOD=2`. Never the engine in `*-Runner.app`.
3. **Xcode Run otherwise:** all four Xcode libs. Breakpoints, MTC, console and view debugger work.
4. **Displaced inserts, in any launch** [M e28, Detox-style]: one line on stderr and in the log, `stuntcam boot mode displaced <tool>; use 'stuntcam up --mode launchd'`. It is not only for launches no rule above covers: a launch-only library that rule 1 or the chain skipped (`tryload`) and that isn't mapped feeds it too, in a hosted test as in a plain launch. The signals are what survives the displacement: Detox's `-detoxServer`/`-detoxSessionId` arguments (`_NSGetArgv`) with no Detox image mapped; `IDB_*` variables; `ASAN_OPTIONS`/`TSAN_OPTIONS` with no matching runtime mapped; Guard Malloc's own settings (`MALLOC_PROTECT_BEFORE`, `MALLOC_FILL_SPACE`, …) without libgmalloc; and a launch-only library skipped above that isn't mapped. Never when the launch's own insert list holds more than our stub (it merged with ours, so nothing was displaced). A signal variable that was in the boot environment (exported as `SIMCTL_CHILD_…` when the sim was armed, recorded as `boot_signals` in `config.json`) is in every process, so it isn't one [e30]. `IDB_*` and globally set `ASAN_`/`TSAN_`/`MALLOC_` variables remain heuristics (TODO). `MallocGuardEdges`/`MallocScribble`/`MallocStackLogging` are libmalloc's and work without an insert, so they are not signals.

**Which Xcode's libs** [M e26, e28; skew itself U4 part 2]:
- **Under ⌘R nothing in the app names the launching Xcode** [M e26]: the variables that survive are DerivedData paths and `IDE_*`; the Xcode root is only in `xcodebuild test`'s `DYLD_FRAMEWORK_PATH`. So the loader can't derive it in-process, and the libs load from the runtime root (`SIMULATOR_ROOT`), which is where Xcode 27.0 loads them from too.
- **The recorded build is the source.** `up`, `exec`, `service` and `env` (and `scripts/boot.sh`) record the Xcode in use at arm time (`xcodebuild -version`, which follows `DEVELOPER_DIR`, else `xcode-select -p`) in `sims/<udid>/config.json` (`proto`, `xcode_version`, `xcode_build`, `developer_dir`, `rules_dir`). They stage `restore-rules/*.json` under `$SIMCAM_HOME/restore-rules/<content hash>/` (a checkout in `~/Documents` is unreadable from the sim) and record that dir as `rules_dir`, so an armed sim's rules never change under it and another clone's or a stale file never feeds the "newest" best guess [e30]. The CLI and `scripts/env.sh` compute the same hash.
- At each launch where a rule fires, the loader reads `<rules_dir>/<xcode_build>.json` (`rules[].libs`: ordered `{path, if, unless}`); a file counts only if it fits the loader's buffer and it and its `rules` value parse (`doctor` and `up` check the same). No record, or no file for that build: the newest build's file (else the built-in 27A266a copy) plus one line, `restore rules unverified for Xcode <build>; if console/debugger misbehave use --mode launchd`. Hosted tests follow XCTest's own list whatever the file says.
- An Xcode switched or updated after `up` is not seen by the app. `doctor` flags it: `DEVELOPER_DIR` ≠ `xcode-select -p` (a `DEVELOPER_DIR` naming the `.app` is the same Xcode), a running Xcode.app that isn't the one in use, and an armed sim whose recorded build or developer dir differs from the Xcode in use. Several Xcodes installed is information, a warning only together with one of these (Xcodes on `/Volumes` count only when in use). `up` again re-records it, with no reboot; `up` refuses a sim boot-armed from another `SIMCAM_HOME` unless `--reboot` [e30].
- `tools/inventory` diffs Xcode's insert list (captured in launchd mode, before the libs remove themselves) against the rules file, so a fifth lib fails the beta ritual loudly [not built yet; TODO].
- Restores run in any process with Xcode markers, extensions included.

**Seeing boot mode from the host** [M e26, e28]: boot-mode arming is invisible to `launchctl getenv` in the sim (every arming variable reads empty). `status` and `doctor` read the booted sim's `launchd_sim` environment on the host instead (`ps` finds the process by its `…/<udid>/data/var/run/launchd_bootstrap.plist` argument; `KERN_PROCARGS2` gives its environment). The stub in `DYLD_INSERT_LIBRARIES` there means boot-armed; its `SIMCAM_SIM_DIR` says by whom. A plain reboot clears it. No process is added to the sim and the stub is unchanged.

**Scheme-level inserts** (Reveal, in-house debug dylibs) are invisible to the stub (A §3.2). `up` scans the focused project's `.xcscheme` files and offers `chain add`, and the README says so.

**Chain file.** The loader reads `sims/<udid>/chain.txt` at each launch [M m11 §1e].
- **Scope:** apps + extensions by default; `scope=all` is opt-in and uses plain `open`/`read`, no CF.
- **Writers:** `up`, `exec`, `service` and `chain add|sync` only. Each entry records its source (`mirrored-from-launchd@<time>`), and stale mirrored entries are dropped at sync. `status` and `doctor` only *report* drift.
- **Loss window:** a T3 panel loader added after `up`, with no later sync, is lost until the next sync, so T3 users are steered to launchd. Recovery of the T3 loader specifically via the chain is [S]; m11 measured a generic library.

**Lazy arming, variant (c)** [M m11 §3], made ordering-safe (A-MF2):
1. The stub acts only in apps: `/Containers/Bundle/Application/`, not an extension or `*-Runner`, `STUNTCAM≠0`, no `disabled` flag.
2. The engine constructor makes **no private call**. It installs the CoreMotion stand-in's hooks, unless a marker from an earlier launch says motion is stock (PR B), and `void*` trampolines on the entry points (today the public capture calls of m11; the **3 entry points** — video-source factory, audio-source factory, session factory — are TODO). Orientation tracking starts at the first motion query or at camera activation.
3. The first trampoline call activates once.

**Activation** (W4, the lazy-activation plan): the first trampoline call starts activation once on a private queue. Callers wait at most 1 s from the first call (×10 under a debugger or sanitizers; `SIMCAM_ACTIVATE_TIMEOUT_MS` overrides it), then forward to stock for the rest of the process. Activation reads the sentinel first, then runs the preflight and resolves every hook. It installs only if no caller has given up. The install is memory-only, sends no messages and takes no dyld lock, and the sentinel's start and logging follow it without anyone waiting. A prepare failure means stock forever. A failure during the install is recorded, so the next launch is stock. Timeouts and crashes during activation count toward the `activate` latch. A latched app skips activation at load, with one line saying so. A per-thread re-entrancy guard forwards recursive calls to stock without waiting.

**Measured costs [M m11]:** 2-7 ms at the first capture call, and a non-camera app is never armed (50/50). New fixtures: CoreMotion queried first; `default(for: .audio)` first; session before discovery; three threads entering at once (U13).

**Costs stated in the README [M m11]:** a reboot to arm or to change the **stub** (rare now); scheme inserts dropped unless chained; MTC restored even when the scheme disables it (`STUNTCAM_MTC=0` turns it off); crash detection blind under the debugger (§5.4).

### 4.2 launchd mode (no reboot; CLI launches only)
(1) Read `launchctl getenv` with stdout and stderr split. (2) **Append** our entry and dedupe by hash prefix and by stuntcam basename. (3) Write, re-read to verify, and retry up to 3× [M m1a, m4 Q1].

Coexists with the T3 panel (both append). Covers icon taps, `simctl launch`, openurl, RN, Flutter, Maestro and Detox (`env`). Xcode Run and ⌘U run unchanged, with no camera (Xcode's per-launch insert list replaces launchd's value [M e32, e41]).

**UI tests: `exec`'s runner hook** [M m4 Q3, e42]. `stuntcam exec -- xcodebuild test|test-without-building` on a launchd-armed or booted-unarmed `-destination` exports `TEST_RUNNER_DYLD_INSERT_LIBRARIES=<libsimcamrunnerhook.dylib>` (the caller's own value kept), the per-run hook dir and the restore-rules file. In the UI-test runner the hook swizzles `-[XCUIApplication _combinedLaunchEnvironment]` (Xcode 27.0: the target-app environment with the test's `launchEnvironment` over it, read by `launch`, `activate` and `launchWithoutAccessibility:`; if absent, `launch`/`activate` themselves) and prepends the stub to the app's `DYLD_INSERT_LIBRARIES`, keeping the test's own entries, plus stuntcam's variables where the test set none (unarmed sims; launchd gives the rest). Measured 27.0 + 26.3: `launch`, `activate`, a test's own insert (both load), 60 frames. The variable **replaces the runner's own insert list**, which hangs hosted tests (e26: no `libXCTestBundleInject`) and aborts an ASan runner; so the hook puts back the restore rules' `test-runner` libs in its constructor (`libXCTestBundleInject` from where dyld finds it, MTC, RPAC) and `exec` puts the toolchain's sanitizer runtime first when the tests run under ASan/TSan (`-enable…Sanitizer YES|NO`; else the `.xctestrun`'s per-target runner insert lists; else the scheme's test plan — `-testPlan` or the default, every selected configuration, options over `defaultOptions` — or, without plans, its test action; ASan measured through the scheme and a test plan, TSan the same path [S]). Because one variable reaches every runner of the run, exec doesn't use the hook (one warning) when the runners would need different runtimes (two sanitizers, or one in only some configurations), when a plan or `.xctestrun` can't be read, when any destination isn't an iOS simulator, or when the Xcode in use has no verified `test-runner` rule (`STUNTCAM_HOOK_FORCE=1` overrides) [e43]. The caller's `SIMCAM_*` settings reach the runner as `TEST_RUNNER_SIMCAM_*`; a scheme/plan with MTC off sets `SIMCAM_MTC=0` there. On a boot-armed sim the hook is inert (the stub is in the runner) and `exec` doesn't export it; forced there, hosted and UI tests still pass. A hosted test's host app gets no camera through the hook (boot mode does that). Any failure: one log line, launch untouched. (B) — `build-for-testing` + editing the `.xctestrun`'s `UITargetAppEnvironmentVariables` — wasn't needed.

### 4.3 Host paths, env, Rosetta
**Host files [M m11 §5].** The app reads config, verdicts, the control file and the chain, and writes status, sentinel and trace files under `~/Library/Application Support/stuntcam/`. Measured on 26.3 and 27.0, every launch path, both modes. On 17.5 the env carries `STUNTCAM_ROOT`/`STUNTCAM_VERDICT`, and writes fall back to container `tmp/` [S]. **Env [M m11 §5]:** `launchctl setenv` vars reach the app on every path; only keys Xcode or XCTest set themselves are overridden. **Rosetta [M m11 §6]** is moot: 26 and 27 sims are arm64-only. On 17.5 the universal stub loads.

### 4.4 Per launcher
| Launcher | Boot mode | launchd mode | Evidence |
|---|---|---|---|
| Xcode ▶ Run | camera; debugger, MTC, console, view debugger restored (not yet re-run on the merged engine) | **no camera**; nothing broken | [M m6 Q2, m11 §1c-d, §2] |
| ⌘U / `xcodebuild test`, hosted | pass (exact inject list, launch-only runtimes skipped); camera in host app, incl. ASan/TSan/UBSan/Zombies schemes [M e26, e28] | pass, no camera (also via `exec`: the hook restores `libXCTestBundleInject`) | [M m11 §1a, e26, e28, e42] |
| UI tests | target armed, runner skipped | `exec -- xcodebuild test`: camera through the runner hook, ASan too (also for booted-unarmed sims); plain `xcodebuild test`: no camera | [M m11 §1b, m4 Q3, e41, e42] |
| `expo run:ios`, `expo start` + `i` | ✓ (cold openurl: "Open in <App>?") | ✓ | [M m1a 1b, m4 Q1] |
| `flutter run` | ✓ (reinstall moves the container) | ✓ | [M m7, m10] |
| `react-native run-ios`, Maestro | ✓ | ✓ | [R internal research §2.2] |
| Detox | **breaks** (its insert is displaced); the loader announces it, one line [M e28, Detox-style launch] | ✓ with `eval "$(stuntcam env)"` | [M m4 Q2, e26, e28]; [R §2.2] |
| CI `xcodebuild` | `exec -- xcodebuild test …` boots a shut-down destination armed | pre-booted (launchd or unarmed): `exec` + runner hook, UI tests only; parallel clones: U19 | [M m4, m11, e42]; runners [S] |

**`up` mode choice:**

| Situation | Mode |
|---|---|
| Sim not booted | boot |
| Sim booted but unarmed | launchd now; a reboot is offered if an Xcode project is in focus or `--xcode` is passed |
| `detox` in `package.json` | launchd |
| T3 loader present | launchd recommended |

`--mode` overrides all of these.

### 4.5 CLI
**`up [--udid U] [--mode boot|launchd] [--reboot] [--source S] [--profile P] [--mic tone|silence|mic] [--grant <bundle>] [--from-source]`** (1) Resolve `DEVELOPER_DIR` and record the Xcode build. (2) Use the release's prebuilt dylibs, or build them locally with `--from-source`. Builds go in content-hashed `builds/<sha>/`, and `current` points to the active one. (3) Pick the sim: `--udid`, else the most recently booted. Say which one and why. (4) **Verdict first.** Use a shipped or cached verdict. Otherwise run the canary on the private-set device (§5.3), before touching the user's sim. (5) Boot armed, with retry/settle and a **health check**: `simctl bootstatus -b` plus SpringBoard up within 60 s. If the check fails: shut down, boot unarmed, launchd-arm, and print why. If the sim is already booted, launchd-arm it and offer the reboot. (6) Apply the `simctl privacy` grants (this terminates the app, so it comes first). (7) Scan `.xcscheme` files. Print "armed until this sim shuts down" and a 5-line doctor summary.

**Other commands:**

| Command | What it does |
|---|---|
| `exec -- <cmd>` | Exports merged `SIMCTL_CHILD_` variables, so sims that boot during `<cmd>` come up armed, and boots a shut-down `-destination id=` armed first. For `xcodebuild test`/`test-without-building` on a launchd-armed or booted-unarmed destination it also exports the runner hook's `TEST_RUNNER_` variables (§4.2) and says so before the run; for any other command it warns there that test launches get no camera. Reports which apps activated the camera and how many launches the hook armed (the in-app reason line: TODO) |
| `status [--json]` | Per sim: mode (boot / launchd / **booted-unarmed**), dylib build, Xcode build, chain drift, verdict. Per app: activation, features, produced/dropped counts, last error, latches with age and reason. Read-only |
| `trace [--json] [--udid U] [--pid N] <bundle>` | **Built (W4 e48).** The 64-entry op ring with host-clock times (shutter → slot 12 → still complete → timestamp), from `trace-<pid>.txt` next to the app's `events.log` (state dir, else the container-tmp fallback); the app rewrites it at most every 150 ms after an op, at exit and from the crash handler, so it reads a running app or the last copy of an exited one. The directory is picked when the camera arms, so a crash right after arming still leaves one; only the first crashing thread writes it; a directory removed or made unwritable mid-run is picked again once (W4 e54). Stages as the engine names them: `shutter` (`capturePhotoWithSettings:delegate:`), `slot12` (Fig slot 12, `IrisStillImageSinkCaptureImage`), `will-capture`, `timestamp` (the photo's `AVCapturePhoto.timestamp`, taken when its source frame is grabbed: 0.1–3 ms after slot 12 unloaded, up to ~20 ms under heavy load [M e48]; a failed photo shows none in the report), `did-capture`, `encode-start`, `encoded`, `still-complete`, `did-finish`; also activate, session start/stop, movie start/stop, latches, crash. `--json` schema in `cli/README.md` |
| `doctor [--json]` | Everything above, plus: Xcode/CLT and multiple Xcodes, `DEVELOPER_DIR`/`xcode-select`/running-Xcode mismatches, a recorded build ≠ the Xcode in use, rules verified or not; launchd_sim's real env (`KERN_PROCARGS2`) [M e28]; displaced and restored libs; step-aside owner; sentinel "unknown" events; mic and TCC; canary-device disk use; compiled-out stacks (VC4, camera-kit, expo-camera <55.0.11, Capacitor, ML Kit without arm64-sim, missing `NSCameraUsageDescription`) with patch links [R internal compat map §0] |
| `service install [--preboot <udid>]` | v1.0, notify-only (§4.6) |
| Also | `source <spec>`, `down`, `disable <bundle>`, `verify`, `sentinel clear\|latch`, `chain add\|ls\|sync`, `env`, `gc` |

### 4.6 A sim that Xcode, Simulator.app, Erase or a reboot booted
Nothing can arm such a sim for Xcode Run without a reboot [M m11 §2]. The README Quick Start orders it: **`stuntcam up` before the first ⌘R**, and again after a Mac reboot, Simulator quit or Erase.

**`stuntcam service` (v1.0, opt-in LaunchAgent, ~150 LOC)** [S]:
- polls `simctl list` every 2 s; when a sim boots unarmed, it launchd-arms it at once (CLI launches work), posts a macOS notification ("Xcode Run won't have a camera: `stuntcam up`") and runs `chain sync`;
- `--preboot <udid>` boots that sim armed at login, before Xcode opens;
- no auto-reboot (v1.1, opt-in).

Without the service, `status` shows `booted-unarmed`, and `up` offers `Reboot <sim> armed for Xcode Run? [Y/n]` (default yes when no app is running). U17 measures how often this happens.

### 4.7 Coexistence
**T3 Device panel (serve-sim loader).** In launchd mode both load [M m1a, m4]. In boot mode it goes through the chain, with the window above. If serve-sim's *camera* capability is on, stuntcam steps aside and names it. **idb:** its per-launch insert owns that launch; in boot mode it is displaced unless chained; an active VideoInjection makes stuntcam step aside. **Upstream proposal:** `SIM_DYLD_CHAIN` / `SIM_CAMERA_OWNER`, plus the engine C API (W6).

## 5. Safety
### 5.1 Preflight gate (all or nothing; covers every hook)
**Checks:** C symbols; the 3 entry selectors with their encodings, plus the ivar; both vtables through a fixed 32-word NULL-tolerant window: version fields, plus every replaced slot by name at its fp-listed index (catches m9's growth and m8's short copy); exported key strings; photo-surface ivar offsets; every client-hook selector and its encoding; the profile, round-tripped through Apple's format parser and read back (non-empty fps ranges, max dims, finite FOV, `description` doesn't throw).

**Behaviour:** strict, so an unknown layout disables the tool [M m10]. A failure gives one stderr line, an `os_log` fault and a status record, then stock behaviour, identical to no dylib [M m6 Q5, m9 Q2]. Cost is 1.4-35 ms, ~120 ms cold on 27.0 [M m10], and lazy arming keeps it off app launch. W4 target: <20 ms, by caching **only the shape half** per (runtime build, dylib sha). The profile round-trip always runs (B-B11) [S].

### 5.2 Fingerprint and verdicts (B-MF3)
**`fp`** = sha256 of what the gate checks: names, slot indices, encodings, key strings, ivar offsets. **No code hashes.** A slot code-hash change only forces a local canary and shows in doctor.

`runtimes/fp-<hash>.json` holds the switches the hash can't see: rotation encoding, attribute and barcode style, mic, Vision revision, interruption map, known canary gaps. **Unknown fp + green gate:** inherit the nearest entry of the **same major runtime**, mark it `inherited`, and let the local canary decide. Switches the canary can't observe (Vision revision, interruption map) get canary probes.

**Verdict key** = (runtime build, fp, dylib sha from the release manifest, profile hash). Verdicts have three states: `pass`; `fail(<check>)`, meaning no camera plus `stuntcam: <reason> — run 'stuntcam verify'`; and `inconclusive(<env reason>)`, meaning the canary couldn't run: timeout on a loaded Mac, disk, missing runtime, CoreSimulator flake. **Never cached**; retried at the next `up` or launch, with one line saying so.

The release ships pass verdicts for its gate-tested runtime builds (C-MF1). `--allow-unverified` opts in per sim.

| Runtime | Status | Basis |
|---|---|---|
| 17.5 21F79 | pass (engine) | matrix [M m10]; no watermark run; fake mic served [M m10] |
| 26.3 23D8133 | pass (engine) | matrix [M m10]; watermark 18/18 per stack [M m12] |
| 27.0 24A434 | pass (engine) | matrix [M m10]; watermark 18/18 per stack [M m12] |
| 18.x, 26.2/.4/.5, 27.1/.2 | unknown | inherited switches + canary; U6, U7 |

The 210-launch soak, TSan torture, lifecycle and two-sim runs have no per-runtime split in m10/m12-result; W1 records it.

### 5.3 Canary-gated enablement
**Where it runs.** On a throwaway device per runtime build, in a **private device set** (`simctl --set ~/Library/Application Support/stuntcam/devices`), so Xcode and Simulator.app never list it [S, U1]: (1) created once; (2) boot-armed headless; (3) prebuilt `dev.stuntcam.canary.app` (~350 LOC) installed and granted camera + microphone only when the device lacks this build's copy (its bundle hash is checked on every run, host side) or its grants (W4 e37); (4) launched once SpringBoard has kept one pid for 2 s and, on iOS 27 and later, no earlier than 8 s after `bootstatus` (the guard and any install count toward it; W4 e38), with a 60 s bound on its RESULT line (its own budget is 45 s). A photo whose only miss is the frame-gap check is retried once at the end of the same launch; a pass that needed it is a *retried pass*, reported by `up`/`verify` and cached for 1 h only. Any other timing miss (start time, frame counts, a late photo) relaunches the whole canary once, and a second miss is inconclusive, never cached (W4 e38); a content fail (`core=FAIL`, not a crash) is confirmed by one more launch before it is cached: a second fail is the verdict, a pass is reported with the fail and cached for 1 h only, like a relaunched pass (W4 e40); (5) the verdict is returned at once, and a detached child shuts the device down and holds the per-runtime lock until it is down (the next verify waits for it; W4 e37).

The canary takes 11-13 s on all three runtimes [M m10], and two sims run without cross-talk [M m12]. First creation is slower (data migration), so `up` shows a progress line. `--canary here` runs it on the target sim (CI). `gc` removes devices for runtimes no longer installed.

**Checks.** Core: discovery, start time, frames through AVF's consumer with retains and IOSurface count flat, photo, movie (incl. a max-duration stop), QR. **Read-backs:** adjusting YES→NO, torch active, bias completion and value, zoom. These matter because a missing key looks idle. With read-backs the canary flagged every m8 mutant that broke a real app; without them it missed 1 of 6 [M m8 B, 26.3, pre-merge copy].

**Regression ledger (`ledger.md`) [S].** One entry per known bug from m5-m12. Each entry is a canary assertion, or a conformance/mutation test where a 15 s run can't see the bug. The count is ~35 (draft: read-backs 4, photo 6, movie/audio 5, formats 5, orientation 5, metadata 3, lifetime 3, runtime shape 4), and it is fixed when the file is written in W2. Every new bug adds an entry plus a mutation variant. Release gate: ledger 100%.

### 5.4 Timeouts, sentinel, latches
**Pause-aware timeouts (B-MF1) [S]:**
- The watchdog thread ticks every 250 ms. A gap >1 s between its ticks means the process was stopped (lldb, suspension, Mac sleep), and **every deadline restarts**.
- While `P_TRACED`, no timeout latches; it records `unknown-hang` and shows the debugger banner.
- A hang latch needs **2 hangs**, or 1 hang whose sample (`thread_get_state` + frame-pointer walk) shows our frames on the blocked thread.
- An op ends when *our* work ends: the still-complete is posted and the marker cleared before the app's delegate runs.
- The stop barrier covers provider quiescence only; writer finalisation is async through didFinishRecording, with a deadline scaled to backlog (queued frames ÷ measured encode fps + 5 s).
- With `__tsan_init`/`__asan_init` present, timeouts are ×10. A latched `startRunning` emits ≤1 runtime error per 5 s.

**Sentinel (B-MF2) [S].** Key = (verdict key, bundle, feature). Two attribution paths. **(a) Op path:** an inflight marker plus a crashing thread in our image, AVFCapture/CMCapture/CoreMedia/CoreVideo/IOSurface/ImageIO/VideoToolbox, or our or AVF's queues. **(b) Provider path, no marker needed:** the crashing thread carries our thread-specific key, or the PC is in our image. It is attributed to the feature that owns the queue. This covers the steady-state frame pump, preview, VDO, ADO and metadata. *W4 PR B implements the key for CoreMotion only:* every stand-in entry point (hooked selectors, timer ticks, operation blocks, the orientation tracker) saves the key, sets `motion` and restores it, and clears it around the app's handler; a fault in our image on such a thread is `crash motion`. A `motion` latch never closes the camera: it makes CoreMotion stock from the next launch (`latched-motion-<build>`, written by the crash handler itself on the second strike).

A chained `sigaction` handler (SEGV/BUS/ILL/ABRT/TRAP) does the attribution **in-process and async-signal-safely**, against image ranges cached at activation, and writes `crashed/<feature>` itself. The next activation latches with no CLI involved. `.ips` files and sim-log exit codes ((0,0,11) crash vs (2,9,9) kill [M m11 §4]) only corroborate, in doctor. App-code top frames never count. A marker without a report stays pending for 10 min.

**Under a debugger,** crash and Stop are identical: no `.ips`, exit (0,0,9) [M m11 §4]. Path (a) gives **unknown**, never counted. The next activation prints `last debug run ended inside <feature> op <n>; if stuntcam crashed: 'stuntcam sentinel latch <feature>'`. Path (b) is recorded silently and shown only if the frame-pump heartbeat stopped >1 s before exit while in the foreground. A plain lldb detach is counted.

**Latching and decay:** 2 attributed crashes within 20 activations or 24 h latch that feature, so old crashes age out. `status` shows the latch's age and reason, the latch line prints `sentinel clear`, and a new verdict key resets it. **Burn-down (C):** if automatic attribution isn't green by the end of W2, v1.0 ships markers + manual latch + doctor, and automatic attribution moves to v1.0.x. Crash-SDK coexistence (Sentry, KSCrash) is U15.

**Mid-session latch = error completion (never stock, never silent):**

| Feature | Behaviour on latch |
|---|---|
| photo | still-complete with an error status |
| movie | stop with failure; file finalised if possible |
| ADO / metadata | stop delivering |
| preview | frozen, stamped last frame |
| session | runtime error, then did-stop-running |

Before any session exists, a latch means stock (no camera). The −11818 and −11819 paths are measured [M m12]. Conformance latches each feature mid-op [S].

### 5.5 Kill switches and step-aside
**Kill switches:** `STUNTCAM=0` (reaches every path [M m11 §5]), `stuntcam down`, `disable <bundle>`, `STUNTCAM_FEATURES=-audio,…`, `STUNTCAM_FIX_<name>=0`, `STUNTCAM_MTC=0`.

**Step-aside.** At activation and at each session start, check the owners of the discovery, default-device, start and add-input IMPs and of our entry points: AVFCapture, us, Xcode's libs or an allow-listed benign image (MTC, Sentry, FLEX …) → proceed; a known camera tool (serve-sim camera, idb Shimulator, SimCam) → step aside and name it; unknown → log and proceed; a stock source that answers attributes without the stub error (Apple shipped a camera) → step aside, unless `--override-apple`.

## 6. Behaviour
### 6.1 Outputs
**Preview.** An IOSurface per frame as layer contents [M m3, m5], with the measured rotation formula and front preview mirrored as on a device [M m12]. Point conversions are Apple code and need the preview-format notification (conformance assertion) [S].

**Data (VDO).** Delivered in the requested BGRA/420v/420f, since AVF converts nothing [R m2bc]. Rotation and mirroring are applied before scaling [M m7]. `exposureDuration` is a finite 1/30 [M m7]; ISO, aperture and WB gains are finite [S]. Flutter ~30 fps end to end [M m7]. Multi-sink 60 fps is U16.

**Photo.** In-sim ImageIO: JPEG 63 ms, HEIC 156 ms at 4032×3024 (standalone) [M m3]. Max dims are honoured [M m8]. **In-pipeline EXIF measured:** DateTimeOriginal + Orientation 6 [M m8], plus 11 unnamed keys [M m5]; Make/Model/Lens and Software are [S] in-pipeline (m3 measured them standalone only). DateTimeOriginal/Digitized, SubsecTime* and OffsetTime* are the frame grab's wall clock, −0.1…+3 ms from the trace's slot-12 mark [M w4 e54]. Host-clock timestamp inside the systemUptime window, plus a preview image [M m8]. **didFinishCapture fires** [M m10]. Back flash [M m7]; `auto` from scene luminance [S].

**Movie.** In-sim AVAssetWriter, `.mov`. Size, fps and codec follow the **active format** [M m8]; `setOutputSettings:` is [S]. Interruption → −11818 + success, file kept [M m12]. **Movie limits in v1.0** (C-MF3) [S]: `maxRecordedDuration`/`maxRecordedFileSize` stop the writer and deliver didFinishRecording with the AVF error + `AVErrorRecordingSuccessfullyFinishedKey=YES`, and `isRecording` flips [R internal compat map :2037]. **4K60 records at 16-31 fps** with the sim's software encoder [M m10]. v1.0 is an honest cap: requested dims, true timestamps, a warning. v1.1 brings host encode (`stuntcamd`; 4K60 transport 0 drops [M m3]). Pause/resume is v1.1, and doctor says so. Metadata passthrough is U18.

**Audio.** **`tone` by default** (or `silence`): no mic input, no audio-session activation, no TCC events from our code [M m12]. Apps' own `.playAndRecord` with the Mac's mic TCC reset is U9. AAC 48 kHz mono for movies and CMSampleBuffers to ADO [M m6, m7]. The A/V start offset of 0.0 ms was measured **with the real mic** [M m6]; for tone it is [S]. **`mic`** is opt-in and requires app authorisation [M m7]. A fake mic is served on all runtimes [M m10]. `AVAudioSession.category` is untouched in tone mode (documented).

**Metadata.** CIDetector QR, plus Vision rev 1 for EAN/Code128 [M m4, m7]. Type list populated on all runtimes [M m10]; EAN-13 digits correct for expo [M m12]. The Vision revision comes from the fp entry, probed by the canary.

**CoreMotion.** Accelerometer and device motion, hooked **eagerly**. Gravity follows device orientation; device motion carries the same gravity with zero user acceleration and rotation rate. **Attitude** (w4 e29): the device held still, upright in the current orientation, yaw 0, in `xArbitraryZVertical`, the only frame `availableAttitudeReferenceFrames` reports (a start with another frame gets the same samples). `quaternion`, `rotationMatrix` and `roll`/`pitch`/`yaw` agree with each other and with gravity under Apple's conventions, measured against CoreMotion's own CMAttitude (q maps device to reference; `rotationMatrix` is its transpose, so gravity = −(m13, m23, m33); q = qz(yaw)·qx(pitch)·qy(roll); `multiply(byInverseOf:)` mutates the receiver; one attitude object per sample) [M w4 e29]. An attitude archives as a plain CMAttitude (class name `CMAttitude`, CoreMotion's own keys `kCMAttitudeCodingKeyQX/QY/QZ/QW`; the archive is byte-identical to Apple's for the same quaternion), so an app that saves a reference attitude decodes it later without the engine, as a real CMAttitude [M w4 e31]. **While the stand-in is active, every attitude decoded in the process is a stand-in**: the engine calls `+[NSKeyedUnarchiver setClass:M5Attitude forClassName:@"CMAttitude"]` when it installs the hooks (not when CoreMotion is stock), so a secure `unarchivedObject(ofClass: CMAttitude.self, …)` of the stand-in's archive, or of Apple's, returns an `M5Attitude` read from CoreMotion's keys. Without it, a decoded real CMAttitude met stand-ins as arguments, and Apple's methods read the argument's private storage, which a stand-in doesn't have: in the 27.0 and 26.3 sims `decoded.multiply(byInverseOf: dm.attitude)` segfaulted in CoreMotion (EXC_BAD_ACCESS at 0x8), and `decoded.isEqual(dm.attitude)` answered NO for the same quaternion (hash different too). With the mapping, the multiply matches Apple's on Apple's objects (|dq| 4e-18), `isEqual:` and `hash` agree both ways, on both runtimes [M w4 e34; motion-probe `m1.*`, `stock.decode`]. Left: a real CMAttitude the app gets some other way (none from the public API while the stand-in is active) still faults when given a stand-in as the argument. `multiply(byInverseOf: nil)` leaves the stand-in unchanged (Apple's CMAttitude faults on nil). The stand-in's `isEqual:` equals any CMAttitude with the same quaternion. A non-unit gravity (the `M5_GRAVITY` override) is reported as given, but the attitude is computed from its direction, so it stays a unit rotation. `magneticField` is zero with accuracy uncalibrated; `heading` is −1. The queue-less `startDeviceMotionUpdates(using:)` and `attitudeReferenceFrame` are stood in too (16 selectors). Gyro and magnetometer report unavailable [M m5, w4 e24]. Device motion stays available (decided in W4): a real iPhone has it and apps may read orientation from its gravity; the stand-in has offered it since m5, and the earlier "deviceMotion unavailable" text was a supposition, never what the engine did. An app's motion recorder (§9 step 3) is to be checked against this in its acceptance run. Several `CMMotionManager` instances at 100 Hz (both ≥97.5%; the engine never fires faster than every 10 ms, so higher requested rates get 100 Hz) and systemUptime timestamps are measured [M w4 e24, U2]; device motion with attitude delivers 100% of 100 Hz [M w4 e29]. The orientation set with `stuntcam orient` (the control file) reaches an app only once its camera arms: an app that uses CoreMotion alone follows the UI and simulator orientation, not `stuntcam orient` (TODO.md).

### 6.2 Orientation, lifecycle, interruptions
**Orientation.** Default: the scene and the accelerometer follow device orientation (Hardware ▸ Rotate). Override with `STUNTCAM_ORIENT` or `source --orient`. **Watermark-verified** [M m12]: plain AVF 36/36 and VC5 36/36, which is back/front × 3 orientations × preview/photo/movie × 26.3+27.0, i.e. **18 per runtime per stack**; expo device-like, QR+EAN decoded 12/12; Flutter portrait 9/9. **Flutter landscape fails** (90° off) on 26.3 and is **untested** on 27.0 (§7). VDO, full-check mode and 17.5 get watermark checks in W5.

**Lifecycle** [M m12; fixture and runtime unrecorded]: background or another camera app → wasInterrupted reason 1; when it ends, frames resume; an active recording ends with its file kept. Only reason 1 is confirmed (reason 3 arrived as 2). A second video session interrupts the first, and an audio-only session may run alongside [S].

### 6.3 Sources
| Source | v1.0 | Evidence |
|---|---|---|
| `synthetic[:seed]` (default): sensor-oriented scene, watermark grid, stamp, **QR + EAN-13** | ✓ | Metal 1.2 ms/frame [M m3]; QR/EAN decoded by expo and mobile_scanner [M m12]; CPU fallback for runners [S] |
| `image:PATH`, `file:PATH` (looped, in-sim decode, ≤1080p) | ✓ | [M m12]; memory growth (U8) |
| `qr:TEXT`, `barcode:ean13:…` (custom payloads) | ✓ | [S]; the drawing path is the default scene's [M m12] |
| live switching / per launch | ✓ | control file <80 ms, no relaunch; `STUNTCAM_SOURCE` via `SIMCTL_CHILD_`, launchEnvironment or launchctl [M m11 §5, m12] |
| `webcam`, `replay`, host 4K encode | v1.1/1.2 | IOSurface ring 4K60, 0 drops [M m3] |

### 6.4 Session-layer rules
(1) **State keying.** State is keyed by (session ptr, generation). Invalidate/Finalize tear our state down first, so a reused address never inherits sinks (the bug m10 fixed). (2) **No synchronous AVF/UIKit calls from providers.** Providers read an immutable snapshot built at configure time. Device orientation comes from CoreMotion's side, or from a main-thread observer that writes an atomic, so MTC stays clean. Debug builds assert "no AVF call from a provider". (3) **Barrier.** The configure/stop barrier waits ≤2 s, measured on pause-aware time (§5.4), then latches. (4) **No retains.** Providers never retain the session. Teardown checks `dispatch_get_specific` before any sync. (5) **Generation check.** Notify blocks re-check the generation. The merged build's notification-posting discipline stays as measured. (6) **Bounded pools.** Pools are VDO 6, preview 3, writer 8. On exhaustion a frame is dropped, counted and reported via didDrop.

Measured: 200 cycles, TSan 0, ASan 0 [M m12].

### 6.5 Profiles and unsupported features
**`iphone-pro-8cam.json`** (new file assembled from the m7/m8/m9 edits; must earn the gate): Triple, Dual and DualWide with constituents; 10 formats in 420v and 420f, 720p-4K at 30/60; photos up to 4032×3024; back torch and flash; `multicam:false`.

`minimal` has 2 cameras and 3 formats. A bad profile means no camera, never an Apple exception. A profile change needs a relaunch.

**Advertised unsupported at the format/attribute level** so Apple's `is…Supported` says NO: Live Photo; depth (measured harmless [M m8]); HDR, ProRAW, responsive/deferred capture, MultiCam. The profile still lists the back **LiDAR Depth Camera** device (as an iPhone 17 Pro does), so apps can offer a LiDAR lens; it behaves exactly like the back wide camera with no depth (no depth formats or port; `isDepthDataDeliverySupported` NO; an `AVCaptureDepthDataOutput` gets no connection and delivers nothing; W4 e62, 26.3/27.0). An app that enables depth without checking gets AVFoundation's own exception, as on every stuntcam camera. Dropping the device waits for the next profile change (the profile's sha keys every verdict and the package).

**Measured and decided in W4 (U3) [M w4 e22, e23a, e23b]:**
- Live Photo, depth, mattes, ProRAW/RAW, zero-shutter-lag, responsive/deferred capture, HDR, HLG/Apple Log, spatial, cinematic and Center Stage already read NO; apps that check never enable them.
- **Live Photo stays unsupported** (owner decision, 2026-11-13). AVCam's shutter crash [M m12] is AVCam's own: it sets `livePhotoMovieFileURL` on every shot without checking that Live Photo is enabled, and AVFoundation throws; a real iPhone always reads YES, so it never shows. Faking YES would give every correct app a Live Photo option that can only fail. AVCam gets a README box and `fixtures/avcam` (a one-line fix plus an Xcode 27.0 build patch). Real Live Photo support can come after v1.0 if asked for.
- Bracketed capture: AVFoundation reports 8 for any camera; the engine forces 0 (a bracket hung after 1 photo). MultiCam: the engine reports no device sets (a multi-cam session failed with −11873); the class flag `AVCaptureMultiCamSession.isMultiCamSupported` is the simulator's own and stays YES, so the device sets are the signal (Apple's MultiCam sample then gets `canAddInput` NO and shows its own "unsupported" path).
- Flash stays offered on the back cameras and never fires (a documented limit): taking it away made apps that set `flashMode = .on` without checking throw at `capturePhoto`.
- 1080p60 movies record at 59.9 fps; 4K60 at ~26–32 fps (the simulator's software encoder), a documented limit.

The canary and `scripts/features.sh` assert these (U3).

## 7. Known gaps, each with a plan
| Gap | Evidence | Plan (v1.0 unless noted) |
|---|---|---|
| Flutter `camera` landscape 90° off (26.3; 27.0 untested) | [M m12] | W4, 2-day timebox. NO-GO: "portrait only" + banner/`status` warning |
| Interruption reason mapping | [M m12] | Post each Fig reason, read the public one, store the map in the fp entry; canary probe |
| AVCam: compile-time placeholder; Xcode 27.0 SDK patch; Live Photo crash (AVCam's own, §6.5) | [M m12, w4 e23a] | Done: README AVCam box; `fixtures/avcam` ships the patch (20/20 shutters on 26.3 and 27.0) |
| Video-source memory 380→414 MB in 4 s | [M m12] | W4 retain audit (U8) |
| MTC always restored under Run | [M m6, m11] | `STUNTCAM_MTC=0`; marker diff (W3); MTC-clean gate (§8) |
| iOS 18.x; Xcode 26.6/27.1/27.2 | — | U6, U4 |
| Hosted runners never run | — | U7: a private scratch repo on the free plan, offered to the user |
| 4K60 at 16-31 fps (full-check mode's 34th check) | [M m10] | v1.0 cap; v1.1 host encode |
| Xcode Run + debugger not re-run on the merged engine | [M m10] | W1 exit test (§10) |
| UIImagePickerController camera | [M m4 Q6d] | v1.2 spike |

## 8. Conformance, CI, release gate
Fixtures run on one build and are content-checked (watermark orientation, dimensions, mirroring). Only content checks count toward the gate.

| Fixture | 17.5 | 26.3 | 27.0 |
|---|---|---|---|
| plain AVF Swift app (14 checks incl. waiting on did-finish; list in `fixtures/plain/README.md`) | ✓ [M m10] | ✓ + 18/18 watermark [M m12] | ✓ + 18/18 [M m12] |
| VC5, audio on/off | ✓ [M m10] | ✓ + 18/18 [M m12] | ✓ + 18/18 |
| expo-camera, audio on/off, QR+EAN | ✓ [M m10] | ✓ (12/12 decoded across 26.3+27.0 [M m12]) | ✓ |
| Flutter `camera` + `mobile_scanner` | ✓ [M m10] | ✓; portrait 9/9, landscape ✗ [M m12] | ✓; landscape untested |
| Full-check mode (VisionCamera 5.2 with a patch, 8-camera profile) | 33/34 [M m10] | 33/34 | 33/34; 34th = 4K60 fps |
| Apple AVCam (SDK-patched) | — | partial [M m12] | partial |
| lifecycle, TSan/ASan torture, sources, two sims | — | ✓ [M m12], runtime unrecorded | — |
| mutation suite (6 variants) | — | ✓ pre-merge [M m8 B] | — |
| canary | ✓ [M m10] | ✓ [M m10] | ✓ [M m10] |
| new: ordering, pause/suspend, concurrency, movie limits, MTC-clean, Expo Go, 18.6, competitor columns | W1-W5 | W1-W5 | W1-W5 |

**CI (planned, never run) [S].** Every PR, on `macos-26` and `xcode-27`: build, `setup-stuntcam`, `exec`, canary, then AVF + VC5 + expo on `synthetic`+`tone`. "No TCC needed" holds only after U9. `verdicts/` is cached. Nightly: Flutter, the mutation suite, a 20× soak. Weekly on the Mac: betas, 17.5, full-check mode, the production app (§9), AVCam, and the Xcode insert-list diff.

**Release gate:** (1) Preflight + canary on 26.3/27.0, and 17.5 fails safe. (2) Content-checked matrix green on the **shipped** profile and **shipped prebuilt dylibs**. (3) 200 boot-armed launches + 200 teardown cycles with 0 attributed crashes, TSan/ASan clean. (4) Mutation suite + ledger 100%. (5) §9 passes on 26.3/27.0, once in launchd mode and once in boot mode with the T3 loader chained. (6) 10 ⌘R with the debugger, plus ⌘U and UI tests. (7) **MTC-clean:** the matrix under Xcode Run with `MTC_CRASH_ON_REPORT=1` shows 0 reports in our frames (B-MF7). (8) U1-U5 GO.

**Gate status (W4):** (3) **GO on the release head's engine `6853b71015f4` (W4 e57, measured internally)**, after the trace ring, the packaging build and the engine hardening (+ e57's follow-ups): 400/400 boot-armed launches and 1000/1000 in-app + 40/40 CLI teardown cycles with 0 attributed crashes, TSan/ASan 0 reports in stuntcam frames on 26.3/27.0 (RN app and plain fixture); the non-passes were timing checks under host load, exact on a low-load rerun. e49 had it GO on `28ff8c0`. (6) and (7) GO on `28ff8c0` (e49). (4) mutation suite 41/41 on 26.3/27.0 (e57). Still open: (1)/(2) on the shipped package, (5) the acceptance test (e55), (8) U4 part 2.

## 9. Production-app acceptance
Besides the fixtures, the release gate runs one closed-source React Native camera app built on VisionCamera, unchanged. The app isn't public, so this section lists the checks, not the app.

**Prerequisites:** Xcode 27; the app's own development setup; Metro running; grants before launch via `npx stuntcam up --mode launchd --grant <bundle>`. One release-gate run uses boot mode with another tool's loader chained.

Then the app's usual simulator run command, with its source unchanged. Clear app data between the ×3 repeats.

**What the checks rely on [R]:** The checks assert only what happens on the simulator and in the app's container; nothing leaves the simulator.

**Data sources:** the photos and movies in the app's container (`exiftool`/`ffprobe`); `stuntcam trace --json` for the shutter mark (`photos[].host_ns.slot12`, `timestamp_minus_slot12_ms`; built in W4 e48).

**Acceptance test.** iPhone 17 Pro on 26.3 **and** 27.0, `--source synthetic --mic tone`, driven by agent-device or Maestro. **Pre-check: U2 passes** (several `CMMotionManager` instances in one process).
1. **Mount:** no camera error. Triple, with zoom switching at the profile's switch-over factors. 4K/1080p/720p at 30/60 offered. Depth UI hidden.
2. **Controls:** zoom + pinch; tap-to-focus resolves; AE/AF lock; the exposure-bias promise resolves in under 1 s and reads back [M m8].
3. **20 photos.** The first shutter comes **≥5 s after mount**. Use a 5-tap burst, flash auto/on/off and each quality setting. Each photo needs: a JPEG at 4032×3024 with EXIF Orientation 6; DateTimeOriginal within 1 s of the host wall clock at the trace's slot-12 mark; will/did-capture once each, plus a preview image; a timestamp within 50 ms of the slot-12 mark; CoreMotion accelerometer samples spanning that timestamp on the systemUptime clock, with gyro and magnetometer reporting unavailable.
4. **Front ×2, mirror on and off:** the watermark flips iff mirroring is on.
5. **Video.** 1080p30 for 5 s, muted and unmuted: `.mov`, 5 ± 0.2 s, 90° transform, AAC iff unmuted, A/V < 50 ms; 1080p60 and 4K30 pass; **4K60 is the known 34th check:** no crash, ~16-31 fps logged, and the step passes on the cap. Torch works. Metadata items are reported but not gated (U18).
6. **Local state:** nothing leaves the simulator, and repeated relaunches leave the app in a consistent state.
7. **Soak:** 20 focus/blur and background/foreground cycles. 0 crashes, RSS growth under 20 MB, IOSurface count flat, every interruption resumes, and no latch from the pauses (U5). This is the first VC5 interruption run.
8. **Kill switch:** `stuntcam down` and relaunch gives the app's no-camera screen with no crash.

**Day 1 for the app:** the camera screen: Triple with zoom switching, focus/exposure/bias, torch and flash; 12 MP JPEG with EXIF, host-clock timestamp and preview image; 4K30, 1080p60 and 1080p30 video with tone AAC, plus movie limits; accelerometer and device motion; deterministic scenes switched live; all on 26.3 and 27.0.

**Not on day 1:** GNSS, depth, landscape, true 4K60, webcam.

## 10. Week-1 checklist (exit = all green, or the burn-down rule applies)
1. **One tree.** Split the prototype into stub (frozen) / loader / engine (fig, providers, client, core) with no behaviour change. Merge m11's restore rules, chain, lazy (c) and `void*` trampolines, then `cloc`.
2. **Exit test (A-MF1, B-MF8).** On 26.3 and 27.0, plus a 17.5 smoke run, in **boot and launchd modes**: the content-checked matrix (AVF, VC5, expo, Flutter; 18/18 watermark per stack per runtime); full-check mode 33/34, where the 34th is 4K60; ⌘U hosted tests and UI tests; 10 ⌘R runs with the debugger, logging first-frame time; 50 non-camera launches with 0 private calls; 20 launches in the m12 TSan torture.

   GO means the counts equal m10/m12. NO-GO: bisect the stub/loader against the engine before W2.
3. **Ordering fixtures** (U2) × 3 runtimes: CoreMotion first, `default(for: .audio)` first, session before discovery, and a two-manager motion recorder.
4. **Swappable loader** (U14). Swap `current`, run 20 launches, and load two builds in one app to check the singleton. Decide on reboot-to-upgrade versus hot.
5. **Mic TCC, app-owned sessions** (U9, 10 min).
6. **U11 probe** (1 h): is there a per-device boot env, which would mean no reboot ritual?
7. **Inventory.** Run `tools/inventory`: fp without code hashes, the exact client-hook count, the Xcode 27.0 insert list → `restore-rules/27A266a.json`.
8. **Evidence record.** Record per-runtime soak/TSan/lifecycle in `ledger.md` and `MATRIX.md`. Copy m10's 14 plain-app checks into the fixture README.
9. **Start the U17 log** (3 working days of re-arm causes).
10. **Ask the user** about a private scratch repo for U7.

## 11. Remaining uncertainties
| # | Item | Test | GO / NO-GO | Blocks v1.0 |
|---|---|---|---|---|
| U1 | Canary: private set, first-ever vs warm, repeatability, busy Mac | `up` with no device or verdict ×3; 10× warm per runtime during an Xcode build | GO: first ≤120 s with progress line, warm ≤30 s, 10/10 same verdict, 0 inconclusive, not listed in Xcode. NO-GO: default set with a clear name; inconclusive retry. **W4 e36 (Go CLI `f1255fe`): NO-GO.** GO for first-ever: `up` takes 62–84 s on 26.3/27.0 and 44 s on 17.5, with the progress line first. GO for same verdict: 75/75 pass. GO for not listed in Xcode. GO for 0 inconclusive on 26.3. NO-GO for warm ≤30 s under an Xcode build: idle 24–30 s, but during an AVCam xcodebuild loop 27.0 takes 31–32.5 s (0/10) and 26.3 7/10. NO-GO for 0 inconclusive on 27.0: 3/37 first attempts hit a photo-hvc1 frame gap, retried to pass at +15 s. Proposals (skip the reinstall of an unchanged canary, return before shutdown, host-side SpringBoard poll, in-canary timing retry) in measured internally, W4 e36. **W4 e37 (those four, `796c597`…`3d02834`): GO on all five.** First-ever: `up` 59–84 s, 40 s on 17.5. Warm: 60/60 ≤ 30 s. Idle: 26.3 18.3–19.4 s, 27.0 23.1–24.4 s. AVCam loop: 26.3 ≤ 24.8 s (median 24.3), 27.0 ≤ 26.3 s (median 25.7). u4diag loop: ≤ 19.5 / ≤ 25.4 s. 76/76 pass; 0 inconclusive and 0 canary relaunches. Not listed in Xcode. Caveat: on 27.0 the canary now starts ~2 s after bootstatus, inside a post-boot window, and a photo stage misses the 100 ms frame-gap check on 27/30 warm verifies (116–886 ms). The new in-canary stage retry passes all 31 of them (e38 correction: e37's L13 fail came from photo-then-stop's content check, not the gap check). measured internally, W4 e37. **W4 e38 (review round 1 fixes, `03123f3`…`5583b50`): GO on all five, and the retry is no longer load-bearing.** The canary launches ≥ 8 s after bootstatus on iOS ≥ 27 (`PostBootSettle`; a 0/5/8/11 s sweep on 27.0 gave 5/5, 7/10 (5 gap misses, 4 frames-A relaunches, 2 runs with both), 0/8 and 1/6 runs with a first-attempt miss; 26.3 needs none: 0/76; e39 correction: this said 9/10). The in-process retry covers only a photo's frame-gap check. A pass that needed it is a *retried pass*: `up`/`verify` print it, and it is cached 1 h. A synthetic first-photo-only stall (`M38_FIRST_PHOTO_STALL_MS=300`) came out as a visible retried pass 5/5, never a plain one. Warm on the final binary: AVCam loop 26.3 20.2–25.0 s, 27.0 26.8–28.8 s (median 27.6; 20/20 ≤ 30 s); idle 26.3 18.6–19.3 s, 27.0 25.3–26.0 s. 0 first-attempt gap misses, 0 retried passes, 0 relaunches, 0 inconclusive in the 50 final and idle verifies. Across all e38 sets: 10 retried passes, not 11, and none in AVCam iteration 3; 2 of them launched ≥ 8 s after bootstatus (g1avbusy-270-1 at 13.6 s, ab11-270-1 at 11.0 s), so at ≥ 8 s the residual retry rate is 2/69 (~3 %), not 0 (e39 correction). Under load the SpringBoard guard's host poll had to stop reading argv (`ps -axww` blocked up to 10.6 s after boot): it now takes the device's launchd_sim pid from launchd (`com.apple.CoreSimulator.SimDevice.<udid>`); the guard is 2.2–4.5 s. First-ever `up` 70–73 s, camera works after. `mutation.sh` 30/30 on 26.3 **and** 27.0; L13 is caught by photo-then-stop and by the gap check missing twice on both runtimes (27.0 by HEVC only, 142/128 ms: a thin margin). Shutdown child: killed if it can't own the lock, re-shuts only while it owns it, lives ≤ 75 s, stale markers taken over; `scripts/verify.sh` waits for it. measured internally, W4 e38. **W4 e39 (review round 2, `18bd6ba`): 27.0 warm under an Xcode build is 29/30 ≤ 30 s, so the warm criterion is not met for 1/30 runs (3 %); the margin is ~0–2 s.** AVCam loop ×30 on 27.0 (load 28–199): min / median / p90 / max 26.6 / 27.9 / 28.8 / 44.7 s. The 44.7 s run relaunched the whole canary: launched 8.0 s after bootstatus (guard 7.1 s with two 2 s `ps` timeouts, settle 0.9 s), its first launch missed frames A (4 frames in 1 s, fps 5.5), and the second launch passed (+15.8 s). Single-launch runs 26.6–29.9 s. A relaunch costs ~16 s, so any relaunch breaks 30 s. First attempts on 27.0: 1 retried pass (hvc1 103 ms) and 1 relaunched pass in 30, both launched at 8.0 s. With e38, 4/99 27.0 warm verifies launched ≥ 8 s after bootstatus needed a retry or relaunch (~4 %): the 8 s floor makes misses rare under load, not absent. 26.3 ×10 in the same loop: 20.1 / 20.7 / 24.4 / 24.6 s, clean. A relaunched pass is now short-lived like a retried one, and `verify` printed its `note:` line. `host-error` guard polls returned under this load (19 in 9/30 runs). The new spawn fallback (r2 L3) never fired, on 17.5 either (all polls `how=host`). **17.5 warm ×10 idle: 18.6–19.2 s, but 1/10 gave `fail(photo-then-stop: didFinishCaptureFor before didStopRunning …)` on the stock engine, and that fail was cached.** That run's `didStopRunning` came before the in-flight photo's completion. Stock-engine 17.5 had one canary run per experiment before this. So 10/10 same verdict is not met on 17.5 (open: an engine stop/photo race on 17.5, and whether a content fail should be confirmed before it is cached). `up` on a fresh sim ran the canary: 26.3 37.0 s, 27.0 44.3 s, plain fixture 14/14 after. `scripts/verify.sh` right after a Go verify waited for the shutdown child, then passed. measured internally, W4 e39 **W4 e40 (photo/stop fix, `w4/photo-stop`): the 17.5 fail was an engine race (ledger L66), fixed; warm 17.5 ×20 20/20 pass (0 false fails), 26.3 ×5 and 27.0 ×5 all pass.** AVF delivers the photo delegate on the main queue and reports the stop from the `stopRunning` thread once its own DidStopRunning handling ran; the two aren't ordered, so the stop could win (17.5: 1/11 canary runs in e39, plain bg stop 8/11 on the old engine). A stop now waits (≤ 1 s, from a private queue) for in-flight photos' didFinishCapture; a deterministic fault (`M40_FINISH_DELAY_MS=300`) failed 9/9 on the old order and passed 9/9 with the fix on 17.5/26.3/27.0. A content fail is now confirmed by one more launch before caching; e2e on 17.5 a first-launch `fail(photo-then-stop …)` followed by a pass came out as a short-lived pass with a `note:` line. measured internally, W4 e40 | **yes** |
| U2 | Hook ordering + two motion managers | fixtures in §10.3, 20 launches × 3 runtimes; two managers at 100 Hz, systemUptime timestamps, gyro/mag unavailable | **GO (W4 e24):** 480/480 armed launches give the same answers lazy and eager on 17.5/26.3/27.0; both managers ≥97.5% of 100 Hz (100 Hz measured; higher rates are capped at 100 Hz); timestamps on systemUptime (max 12.3 ms off); gyro/mag unavailable; device motion available by decision (§6.1; attitude not stood in, an app's motion-recorder check open); ordering fixtures 100/100 on 17.5. **W4 e55 (production-app pre-check, launchd):** motion-probe 12/12 per runtime on 26.3/27.0, both managers ≥ 99.3 %, gyro/mag false, 0 non-monotonic; the production app's recorder with device motion available writes accel + a device-motion stream with zero rotation/user-accel/mag (40/40 files). **W4 e61 (final code, ×3):** motion-probe 12/12 per runtime, both managers ≥ 99.5 %, gyro/mag false, 0 non-monotonic; the production app's motion recordings 140/140 with accel covering the window and no gyro/mag streams | done |
| U3 | Unsupported-feature advertising | AVCam defaults, 20 shutters; canary reads NO for Live Photo/HDR/depth/ProRAW | **Resolved (W4, §6.5):** everything undeliverable reads NO except flash (offered, never fires) and the simulator's MultiCam class flag; AVCam's crash is its own and is fixed by `fixtures/avcam` (20/20); error-complete Live Photo rejected | done |
| U4 | Boot residue + Xcode skew: sanitizer/Zombies/backtrace schemes, idb, Detox line; Xcode 26.6/27.1/beta side by side; Xcode update with sim booted | each ×3 on 26.3/27.0; dump surviving markers | GO: launching Xcode identified, libs restored or clearly announced. NO-GO: launchd default for unknown Xcode builds; mismatch line. **Part 1 (Xcode 27.0) GO after W4 e28** (e26 found it not GO): hosted `xcodebuild test` 18/18 with the camera (ASan 6/6, was 0/6; TSan 6/6 with one runtime image; default 4/4; UBSan 2/2), ⌘R 10/10 on 5 schemes with the 4 libs restored, Detox-style launches announce `displaced` exactly once (6/6; 0/4 plain launches, 0/10 ⌘R, 0/19 hosted), a build without rules gets the "unverified" line with the camera and restores intact (5/5), the Xcode build is recorded at arm time (⌘R can't name it in-process) and `doctor` flags a `DEVELOPER_DIR`/recorded-build mismatch. **Re-checked after review round 1 on main's engine (W4 e30):** hosted 16/16 (ASan 6/6, TSan 6/6 one image, default 2/2, UBSan 2/2), ⌘R ASan 2/2, Detox line 4/4, plain 0/4, unverified line 3/3 with a faked build and 0 with the real one; restore rules staged per content (two homes keep their own `rules_dir`). **XCUITest under ASan fails in boot mode (0/2)**: the UI-test runner's ASan runtime comes only from Xcode's per-launch insert; the loader says `displaced AddressSanitizer` once, then the runner aborts ("Interceptors are not working") — a boot-mode limit, documented in TROUBLESHOOTING. In launchd mode those tests run but with no camera (W4 e32: Xcode's per-launch insert replaces launchd's), so no mode gives sanitizer UI tests the camera yet. **Part 2 pending**: a second Xcode (27.1/26.6) and an update with a sim booted need the owner's Apple ID download; idb untested (not installed) | **yes**, for the boot default (part 2) |
| U5 | Pause/suspend vs timeouts | 10 s breakpoint in photo, KVO and VDO delegates; background mid-photo/recording; Mac sleep 1 min; 4K60 30 s stop | **GO (W4 e25):** 22 runs on 27.0 and 26.3, 200 photos + 40 recordings, 0 latches, 0 counted hangs, 0 crashes; every pause shows as a watchdog gap with deadlines restarted (lldb 10 s holds, backgrounding mid-photo/recording (ops open across the suspension on 26.3 only), 60 s SIGSTOP as the Mac-sleep stand-in (the case that really exercises the restart; real Mac sleep untested), 30 s 4K60 stop in 0.18–0.38 s). Recordings have no deadline (none needed for pauses); a recording across a freeze holds one frame for the freeze; the "expired op under a debugger is never counted" path was not exercised (lldb also stops the watchdog). **W4 e55 (production app, VC5, §9 step 7):** 20 focus/blur + background/foreground cycles per runtime: every interruption resumed (42/41 and 41/40 session starts/stops), 0 latches, 0 crashes. **W4 e61 (final code):** 9 soaks (×3 Debug per runtime, boot mode, one Release per runtime), 20/20 resumes each, 0 latches, 0 crashes | done |
| U6 | iOS 18.6 + 17.5 watermark | full matrix | GO: pass. NO-GO: inherited/unverified via gate. **17.5 watermark: plain 18/18 (W4 e46).** **18.6: not measurable with Xcode 27.0 (W4 e56).** `xcodebuild -downloadPlatform iOS -buildVersion 18.6` (and `22G86`) says "not available for download", though Apple's index lists 18.6 `22G86`. 18.x stays unverified, behind the gate (`verify` + preflight). Measuring it needs another Xcode that offers it, or the Apple-ID download (owner's call). measured internally, W4 e56 | no |
| U7 | Hosted runners (private scratch repo) | PR job 10/10, `macos-26` + `xcode-27`, Metal/CPU synthetic | GO: O6/O10 +1. NO-GO: self-hosted | no (user decision) |
| U8 | Video-source memory | 10 min `file:` 1080p, RSS + `leaks` | GO: flat ±20 MB. NO-GO: `file:` experimental ≤720p | `file:` only |
| U9 | Mic TCC with app-owned sessions | reset CoreSimulator mic TCC; VC5 audio, expo unmuted, Flutter, tone | GO: no hang. NO-GO: doctor check + "grant once"; CI claim stays [S] | CI claim only |
| U10 | Point-release fp inheritance | 26.5, 27.1, 27.2 on the Mac | GO: same or inherited fp, canary passes, no edits. **W4 e56: GO for 27.2 (24B8117).** Preflight passed on every sampled 27.2 activation with 27.0's 24-slot session layout, so the same shape as 27.0 (no runtime fp tool exists yet; preflight is the fp check). `verify` 5/5 pass. The matrix is exact in launchd mode; in boot mode exact except `vcfull` fps misses in 4/8 runs, all under heavy host load, where an interleaved 27.0 control also missed a 1080p60 fps check (both exact at low load). Plain orientation 18/18 ×2, `conformance.sh` 6/6, plain soak 20/20, no engine edits. **26.5 not measured** (the download would have taken free disk below 40 GB); 27.1 not measured (superseded by 27.2). 27.2 also ships as `24B91016s`, an iPhone19,4-only runtime with no device type under Xcode 27.0. `verify` reports it inconclusive and doesn't cache it. measured internally, W4 e56 | no |
| U11 | Auto-arm Xcode-booted sims | 1 h probe (W1) | GO: no ritual, O2 +1. NO-GO: §4.6 stands | no |
| U12 | Crash attribution in-process, paths (a)+(b) | provider crash ×5 per feature; Xcode Stop ×10 | GO: latch on 2nd, 0 false banners on Stop. NO-GO: burn-down rule (§5.4) | no (burn-down) |
| U13 | Concurrent first-call activation | 3 threads, VC5 + expo in one app, 100 launches, TSan | GO: 0 deadlocks, one activation. NO-GO: async pre-arm on first runloop turn. **W4 e57 (item 3 re-run, not this test):** 100 TSan RN launches (VC5 + expo, 1000 cycles) on the release engine, 0 reports in stuntcam frames, 0 deadlocks; the stop waiter now runs on a serial queue per session (overcommit), so a start's flush can't starve on an exhausted GCD pool | no (W2) |
| U14 | Swappable loader + singleton | §10.4 | GO: no-reboot upgrades. NO-GO: reboot-to-upgrade + `proto` skew warning | no (decide W1) |
| U15 | Sentinel with a crash SDK | a VisionCamera fixture + a crash-reporting SDK (e.g. Sentry); provider crash ×5 | GO: `crashed/<feature>` 5/5. NO-GO: orphan-marker inference + doctor `.ips` scan | no |
| U16 | Multi-sink 60 fps | preview + VDO + movie 1080p60; 200 ms consumer | ≥58 fps, didDrop, RSS flat | no |
| U17 | Re-arm frequency | 3 days of real use | ≤1/day: README + prompt suffice; else push `--preboot` as default advice | no |
| U18 | Movie metadata passthrough | VC5 + location; `ffprobe` | GO: `software` + ISO6709. NO-GO: pass settings metadata (v1.0.x). **W4 e55 (production app, VC5 + location granted): NO-GO as observed:** every movie carries `creation_time` only (12 clips, 26.3/27.0); the same in W4 e61 (46 clips) | no |
| U19 | Parallel-testing clones | `exec -- xcodebuild test -parallel-testing-enabled YES` | GO: UI tests armed on clones. NO-GO: document "disable parallel testing" | no |
| U20 | Back camera preferred via Fig source order | AVCam + `systemPreferredCamera` | GO: back. NO-GO: doc + async one-shot setter | no |

## 12. v1.0 scope, schedule, maintenance
**v1.0 contents:**
- **Core:** frozen stub, swappable loader, singleton engine; gate, fp/verdicts (tri-state, inherited), canary in a private set, ledger, pause-aware sentinel and latches.
- **Behaviour:** synthetic/qr/barcode/image/file sources with live switching and per-launch env; movie limits; two profiles.
- **CLI** as in §4.5, including `trace` and the notify-only `service`. **Packaging:** prebuilt dylibs, canary app and verdicts in npm/tap; `setup-stuntcam`.
- **Tests:** fixtures and the production-app acceptance test (§9). **Docs:** README per persona, with "`stuntcam up` before ⌘R" and "no webcam until v1.1" on screen one; `ARCHITECTURE.md` (Fig primer); support policy; issue template requiring `doctor --json`; `THIRD_PARTY_NOTICES.md`.

**Cut to v1.1:** auto-reboot, MCP, `stuntcamd` (webcam, host encode), the engine C API, pause/resume.

**Estimate: 7 weeks (6 + 1 contingency) [S].** Round 4 adds about 4-5 days: the service (1 d), movie limits (½ d), `trace` (½ d), the loader split (½ d), and the pause/sentinel/fp spec in W2 code (1-2 d). That uses most of the contingency, so two burn-down rules apply: (1) If W1 spills, move `file:` and the competitor columns to v1.1. (2) If sentinel auto-attribution isn't green by the end of W2, ship manual latch + doctor.

| Wk | Work |
|---|---|
| 1 | §10 checklist |
| 2 | Verdicts (tri-state, inheritance, shipped); private-set canary (U1); ledger + mutation suite; sentinel (a)+(b), decay; pause-aware watchdog (U5); error-completion latches; status/trace; U12, U13 |
| 3 | CLI incl. `service` + `--preboot`; Xcode-keyed restore rules + insert diff; chain sync/scope; scheme scan; launchd write-verify; runner hook (built W4 e42); boot-health fallback; gc; npm + tap with prebuilt artifacts; U4 |
| 4 | Unsupported features + AVCam (U3); movie limits; CoreMotion fixes from U2; Flutter landscape timebox; video memory (U8); interruption map; preflight <20 ms; 4K60 messaging |
| 5 | Content-checked matrix on 17.5/26.3/27.0 + 18.6 + point runtimes (U6, U10); MTC-clean gate; Expo Go; competitor columns; production-app acceptance (§9) in both modes |
| 6 | Docs; release gate; timed human port 26→27 on the split tree; raise the engine C API with serve-sim; runners if a repo is approved |
| 7 | Contingency |

**LOC [S]:** sim ≈3.3k (prototype 1.8k + loader/gate/sentinel/latches/status ≈1.5k); CLI ≈1.7k, including the service; canary 350 [M m8]; tools 450; conformance ≈1.8k (600 already measured [M m10]).

**Yearly maintenance.** The basis is the m9 port: 26→27 took +34/−9 LOC and 1 key, absorbed by Apple's initializer, and 17.5→26 needed one offset and one silent semantic [M m2, m6, m9]. Agents did those ports, so a human should budget 2-5 d (B). Fp inheritance, the insert-list diff, the swappable loader and prebuilt dylibs all cut point-release and Xcode churn.

| Item | Year 1 | Later |
|---|---|---|
| major runtime port | 2-5 d | 2-4 d |
| beta season (inventory + insert diff, canary) | 3-5 d | 3-5 d |
| point releases (6-8/yr): inherited fp + local canary | ~1 d | ~1 d |
| Xcode restore rules (per-build file) | 1-2 d | 1 d |
| fixture churn (5 stacks) | 7-10 d | 5-7 d |
| ledger/canary upkeep + triage (support policy) | 6-8 d | 5 d |
| **total** | **≈4-5.5 wk** | **≈3-4 wk** |

**Single-maintainer plan.** Per-PR CI runs AVF/VC5/expo; Flutter runs nightly; the production-app test (§9) stays local. The published matrix is the alarm, and the ledger means each bug is fixed once. `tools/inventory-diff`, the Fig primer and the W6 timed human port cover the bus factor. A June beta ritual, and a support policy (five stacks; "doctor says compiled out" → upstream). "No camera, with a clear message" keeps a missed port safe.

## 13. Honest risks
- **[M] Semantic drift can pass shape checks.** 17.5's rotation change was silent [M m6], and the core canary missed 1 of 6 mutants [M m8]. Read-backs, the ledger, fp inheritance with a canary verdict and per-build verdicts narrow this; they don't close it.
- **[M] Boot mode depends on Xcode's private insert list** [M m11]. Per-Xcode rule files and the insert diff make breaks loud, not absent.
- **[M] Crash attribution is blind under the debugger** [M m11]. Timeouts are pause-aware, but every safety mechanism in §5.4 is [S] until W2.
- **[S] Bugs in the safety machinery show up as a false "no camera".** `doctor --json` is the triage path.
- **[R] Compile-time gates can't be fixed at runtime** (VC4, camera-kit, AVCam's placeholder).

## 14. Self-score (0-5 × weight; max 140; conservative; round-4 evals 100/100/100)
| Obj (w) | S | Reason |
|---|---|---|
| O1 Zero-touch (3) | 4 | Nothing enters the app; ⌘U, UI tests, console and view debugger survive boot mode [M m11]. Not 5: Xcode-booted sims need `up` + reboot; Detox via `env`; scheme inserts need `chain add` |
| O2 Install/use (3) | 3 | Prebuilt artifacts, shipped verdicts and the notify-only service fix first-run and silent-miss on paper [S]; nothing installable yet; reboot ritual remains [M m11 §2] |
| O3 Fidelity (3) | 4 | Genuine objects; 5 stacks × 3 runtimes on one engine [M m10]; 18/runtime watermark, lifecycle [M m12]; movie limits added [S]. Against: Flutter landscape, AVCam partial, picker, interruption reasons |
| O4 Real outputs (3) | 4 | Preview, VDO, JPEG/HEIC + host clock + preview image + did-finish, AAC, ADO, QR/EAN [M m6-m12]. In-pipeline EXIF only partly measured; 4K60 capped; no depth/Live Photo |
| O5 Safe/robust (3) | 4 | 210 launches 0 crashes [M m10]; TSan/ASan [M m12]; gate fails safe [M m9]. A weak 4: arming+engine integration, pause-aware timeouts, sentinel paths and fp inheritance all [S] until W1-W2; AVCam crash (U3) |
| O6 Maintainable (3) | 3 | Small measured ports; fp inheritance, per-Xcode rules, insert diff, swappable loader [S]. Hosted CI never run |
| O7 Formats (2) | 4 | 10 formats, 420v/420f, 4032×3024, virtual devices [M m5, m8]; 4K60 capped |
| O8 Performant (2) | 3 | 2-7 ms arm; 6-9% CPU; multi-sink 60 fps unmeasured; video source 24% CPU + growth |
| O9 Sources (2) | 4 | Scene with QR+EAN, image, file, live <80 ms, per-launch env [M m12] |
| O10 Automatable (2) | 3 | `exec`, `--json`, `trace`, control file, two sims [M m12]; MCP deliberately v1.1; runners unproven |
| O11 Small/legible (2) | 3 | ≈3.3k sim LOC in stub/loader/engine modules; Apache-2.0; primer unwritten |
| **Total** | **100/140** | Unchanged: round 4's fixes are design until measured. Expected after the W1 exit test + U1-U5 GO: O2 → 4 (+3, with the service shipped), a solid O5 4. Runners green: O6, O10 → 4 (+5). ≈105-108 |
