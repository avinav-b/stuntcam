# Conformance matrix (W1 exit test, 2026-11-12)

Xcode 27.0 (27A266a), macOS 26.6, iPhone 17 Pro (iPhone 15 Pro for 17.5). Lazy arming on unless noted; "eager" =
`SIMCAM_LAZY=0`. Counts are PASS/total. Details, commands and raw logs are kept internally.

**Which build.** The week-1 runs (e1–e7) each tested the tree as it was then, so the rows below come from several engine
builds: the app matrix and orientation rows (e1b/e2/e3a) and the soak and torture (e3b) predate the watchdog and sentinel,
the CoreMotion-at-load change and the stub/loader split; e7 is the only run with the loader before the final review fixes.
The final run on the exact commit merged to `main` is **e14** (section at the end); where it and an earlier row disagree,
e14 is the evidence.

## Apps (design §8)

| Fixture | 17.5 boot | 26.3 boot | 26.3 launchd | 27.0 boot | 27.0 launchd |
|---|---|---|---|---|---|
| canary (core + ext) | ✓ (after `7a3bcf4`) | ✓ (after `d3e5f66`) | ✓ | ✓ | ✓ |
| plain AVF | 14/14 | 14/14 | 14/14 | 14/14 | 14/14 |
| VisionCamera 5, audio off / on | 17/17, 18/18 | 17/17, 18/18 | 17/17, 18/18 | 17/17, 18/18 | 17/17, 18/18 |
| expo-camera, audio off / on | 8/8, 9/9 | 8/8, 9/9 | 8/8, 9/9 | 8/8, 9/9 | 8/8, 9/9 |
| Flutter `camera` + `mobile_scanner` | 18/18 | 18/18 | 18/18 | 18/18 | 18/18 |
| *(historical)* the removed full-check harness (VisionCamera 5.2 with a patch, 8-camera profile) | 33/34 | 33/34 | 33/34 | 33/34 (one run 32/34: 1080p60 at 53.6 fps) | 33/34 |
| `vcfull` (generic replacement, latest VisionCamera) | e14 | e14 | e14 | e14 | e14 |

The 34th check is 4K60 recording, 16–32 fps with the simulator's software encoder (known, design §6.1). Eager runs
(26.3, 27.0, 17.5 canary) gave the same counts. Sources: e1/e1b (26.3), e2 (27.0, 17.5), e3a (17.5 re-check, launchd).

## Orientation (watermark-checked)

| Stack | 17.5 | 26.3 | 27.0 | 27.0 launchd |
|---|---|---|---|---|
| plain AVF (back/front × 3 orientations × preview/photo/movie) | 18/18 (was 14/18 before `7a3bcf4`) | 18/18 | 18/18 | 18/18 |
| VisionCamera 5 | — | 18/18 | 18/18 | 18/18 |
| expo-camera: QR + EAN-13 decoded (not an orientation check) | — | 6/6 | 6/6 | — |
| Flutter portrait | — | 9/9 | 9/9 | — |
| Flutter landscape | — | ✗ 90° off | ✗ 90° off (first test) | — |

## Arming, ordering, safety

| Check | Result | Source |
|---|---|---|
| Lazy arming: one activation per camera app | 3–37 ms, all runtimes | e1b, e2 |
| Non-camera app: loaded, never armed | 50/50 (26.3), 20/20 (27.0), 20/20 (17.5) | e1b, e2 |
| Chain file: later library loads without reboot; own library skipped | ✓ 26.3 | e1b |
| launchd mode coexistence (pre-existing loader kept, no duplicate, `--off` removes only ours) | ✓ 26.3/27.0 | e3a |
| Kill switch `SIMCAM=0` | no camera, no crash | e3a |
| Ordering fixtures (motion, motion2, audio, session, threads) | 100/100, 26.3 + 27.0 | e5 |
| Watchdog: 2 hangs latch; 10 s SIGSTOP and 10 s breakpoint give no hang | ✓ | e5, e4 |
| Sentinel: own crash attributed and latched after 2; `abort()` elsewhere not counted; clear | ✓ | e5 |
| Stop inside an op under Xcode: reported, not counted | ✓ | e4 |
| TSan: watchdog/ops and tone code | 0 engine reports (after `a606a48`) | e5, e4 |
| Reproducible build (same source → same hash) | ✓ (after `2b5b4cb`) | e4 |

## Xcode-launched (design §10 item 2)

| Path | Result | Source |
|---|---|---|
| `xcodebuild test`, hosted (exact `XCTestBundleInjectPath` restore) | 20/21 (e4), 26/30 (e6): host launched via a `containermanagerd/Dead/` hard-link alias failed the stub's app-path test; fixed in `2aee441`: 30/30 (e7; the alias appeared in 5 of them) | e4, e6, e7 |
| `xcodebuild test`, UI tests (runner skipped, target armed) | 4/4 | e4 |
| Xcode ⌘R with debugger (LogRedirect, MTC, RPAC, ViewDebugger restored) | 10/10; first frame 1.16–1.56 s (6.4 s on the run that built) | e4 |
| Console (print, NSLog, os_log, stderr), Capture View Hierarchy | ✓ | e4 |
| ⌘U hosted + UI | ✓ | e4 |

## Stability

| Check | Result | Source |
|---|---|---|
| Crash reports across all W1 runs (e1b, e2, e3a, e4, e3b soaks) | 0 from our apps outside deliberate sanitizer/crash tests | — |
| TSan teardown torture (VC + expo cycle modes, sanitizer-built app, 20 launches × 10 cycles) | 26.3: 200/200 cycles, 0 reports; 27.0: 199/200, 2 reports both inside VisionCamera's orientation handler (no engine frame) | e3b |
| ASan teardown torture (26.3, 10 launches) | expo 50/50 cycles, 0 errors; VC path blocked by an ASan abort inside VisionCamera's C++ bridge on first configure (no engine frames) | e3b |
| Soak, boot-armed, all 8 apps | 26.3: 200/200 launches, 199 as expected (1 fps flake in the removed full-check harness, passed on rerun), 0 crashes, RSS flat (±5%); 27.0: 50/50, 0 crashes | e3b |

## Uncertainties probed in W1 (design §11)

| # | Verdict | Evidence |
|---|---|---|
| U7 hosted runners | GO with caveat (below) | measured internally (W1 U7) |
| U9 mic TCC, app-owned sessions | sim level: no hang in 11 runs (VC5 / expo / Flutter × revoked / not determined / granted; tone mode). Mac level: not run, `tccutil` can't reset the Simulator client alone | measured internally (W1 e6) |
| U11 per-device boot env | NO-GO for Xcode Run: a LaunchDaemon in the device's data dir sets env 1.8–22 s after boot, never under an xcodebuild boot, and per-launch inserts override it; host `launchctl setenv` doesn't reach Xcode-booted sims. §4.6 (`up` before ⌘R, notify-only service) stands | measured internally (W1 e6) |
| U14 swappable loader | GO: no-reboot swap 20/20, proto skew + 6 broken `current` cases refused cleanly, duplicate engine ignored, launchd mode, stub/loader libSystem-only; decision: hot upgrade (no reboot) | measured internally (W1 e7) |

## GitHub-hosted runners (U7)

| Runner | Xcode / runtime | Result | Source |
|---|---|---|---|
| `macos-26` (virtual M2 Pro) | 26.6 / iOS 26.5 | build + armed boot OK in every job; plain 27/27 after the window fix; canary 20/23 overall, failures all runner timing | measured internally (W1 U7) |

## Known gaps carried to later weeks
Flutter landscape (W4 timebox); 4K60 fps cap (v1.1 host encode); canary's first launch after a fresh boot sometimes gets no
frames for ~7 s (e5; also on the pre-W1 engine); `--tsan` engine can't be loaded by the stub (TSan must load at process
start: sanitizer runs use sanitizer-built apps); the hosted-test hang is fixed (see Xcode-launched). Not covered in week 1:
17.5 watermark checks for VisionCamera and expo, ordering fixtures on 17.5, the U17 re-arm log (TODO.md).

## Size (code lines, no blanks or comments, design §12 estimate in brackets; `wc -l` with comments: stub 58)
Recounted at `7e62160` (e14; method kept internally): stub 49 (≤62) · loader 102 · engine 2318
(prototype 1.8k + loader/gate/sentinel ≈1.5k planned): fig 469, providers 842, client 308, core 670 (safety 238), `simcam.m` 29
· canary 394 (350) · fixtures plain 226, plain-orient 173 · tools 561 (450) · scripts 427.

## Week-1 exit verdict
Not all of design §10 is green: the items in `TODO.md` "Known departures" are open (lazy-activation timeout, CoreMotion outside
the gate, 17.5 ordering/watermark coverage, U17). The exit test proper (§10 item 2) is decided by e14 below.

## Final run on the merge commit (e14)
Commit `7e62160` (the week-1 head after review round 4; only `docs/` changed after it), one build: loader+engine **H =
`c9263d86057c`**, stub `09908f5dbe40`, identical over two builds. Every `stub.log` names H's loader and every activation logs
build H: 173/173 normal launches in part 1 and 211/211 in part 2. The only other launches were deliberate (one swap, 20
TSan, 10 refusal cases). Details, commands and raw logs are kept internally.
The two parts ran at the same time on separate sims, so the Mac was heavily
loaded (load average 300-600): e14's timings (first frame, Xcode frame counts) aren't comparable with earlier rows.

| Area | e14 result | |
|---|---|---|
| Apps, boot mode, 26.3 + 27.0 | canary core+ext PASS, plain 14/14, VC 17/17 and 18/18, expo 8/8 and 9/9, Flutter 18/18, `vcfull` `known=4 na=3 unexpected=0 total=102`, `vcfull-audio` `total=109` | PASS |
| Apps, launchd mode, 26.3 + 27.0 | same counts; `vcfull` needed its one fps rerun on both runtimes (below) | PASS |
| 17.5 smoke, boot | canary, plain 14/14, VC 17/17, expo 8/8, Flutter 18/18 with QR + EAN-13 decoded; `vcfull` exact too | PASS |
| Orientation, boot | plain 18/18 on 17.5, 26.3 and 27.0; VC 18/18, expo QR+EAN 6/6, Flutter portrait 9/9 on 26.3 and 27.0 | PASS |
| launchd arming | foreign `DYLD_INSERT_LIBRARIES` entry kept, no duplicates on re-arm, `--off` unsets both arms' variables; `SIMCTL_CHILD_DYLD_INSERT_LIBRARIES` ignored with a warning; injected-failure rollback (see below) | PASS with a departure |
| Swap, broken `current`, proto skew, kill switch | swap to a second build and back without a reboot; 10/10 refusal cases, no camera, no crash | PASS |
| Xcode | hosted tests 30/30 (5 with DerivedData in `/tmp`), every inject entry loaded (6 runs; dyld maps Xcode's own `libXCTestBundleInject.dylib` through `DYLD_LIBRARY_PATH`, because the entry's path as written doesn't exist on disk, the same as without stuntcam); UI tests on 26.3 and 27.0; ⌘R with the debugger 10/10, Xcode's libraries restored every time | PASS |
| Safety | `-[NSObject init]` still in libobjc, `AVCaptureSession` has its own trap; crash under lldb ×2 = `crash-traced`, no latch; ×2 without = latched, then cleared; non-camera app 50/50 never armed; ordering fixtures 100/100 | PASS |
| Stability | `vcfull` ×30: 30/30 exact, 0 flakes; TSan torture 20 launches (200 cycles), 0 reports; soak 50 launches over all 8 apps: 50/50, 0 crash reports with our frames, 0 `crash`/`hang` events; peak RSS has no consistent trend (each launch is a new process; canary and plain vary ±15% and their last-third means are higher, 863→936 and 895→1047 MB; the other apps stay within ±4%) | PASS |

**Departures from the acceptance bar, each judged before merge:**
- **fps flakes.** Launchd `vcfull` failed `vcf.video.4k30.fps` (22.4 and 25.7 fps) on both runtimes in the same minute, while
  both ran together with a TSan compile at load average 367. Under the pre-registered policy each got one rerun; both were
  exact at 30.0 fps, and the 30 `vcfull` runs afterwards had no fps failure.
- **`scripts/conformance.sh` isn't on this branch** (it arrives with the later PR that adds it). Its loader cases were run by
  hand with the same method: 10/10. Its `motion-gate` and `latch-photo` cases weren't run; `crash-latch` is covered by part 2.
- **Rollback test.** With the `xcrun` shim refusing `setenv SIMCAM_CHAIN` on a re-arm, `launchd-arm.sh` didn't roll back: the
  variable already held the wanted value from the previous arm, and the script checks the value it reads back, not the
  exit status. The sim ended correctly armed on H. With a chain value that differs, the same refusal rolled back as
  required (foreign entry kept, no stub left, previous `current` restored). The 9 variables the earlier arm had set stay set;
  without the stub in `DYLD_INSERT_LIBRARIES` nothing reads them.
- **Crash reports.** `tools/crashes.py`: 0 with our frames on part 1's sims, and exactly the 4 deliberate test crashes on part
  2's. Of the 17 `.ips` files written during part 1, 7 are simulator daemons on part 1's sims during boots and shutdowns, with
  no stuntcam frame on the faulting thread (only the faulting thread was checked); 4 are part 2's deliberate crashes; the other
  6 name no part 1 sim and weren't examined. Part 2's sims were checked only with `crashes.py`, which doesn't list daemon
  crashes (TODO.md).
- **Third-party app names.** No private app's name appears in the code (checked at review).

**Exit test (design §10 item 2): passed on `7e62160`.** The earlier rows above are superseded by e14 wherever e14 covers
them; rows e14 doesn't repeat keep their original run: the U-items, CI, 17.5 ordering, the 17.5 audio variants, 27.0 orientation in
launchd mode, Flutter landscape and the eager (`SIMCAM_LAZY=0`) runs.

## Webcam source (1.1, 2026-11-16)

Engine + loader `cf75da88e48f`, stub `09908f5dbe40`, helper
`5380950f5c94` (Apple Development-signed, already authorized: no prompt). Boot mode, iPhone 17 Pro (iPhone 15 Pro for
17.5). **R** = the real camera, the MacBook Pro's built-in one, with the helper's latency stamp drawn into the frames (the
picture itself was black); **S** = the helper's synthetic pattern. The webcam was the source of every launch
(`SIMCAM_SOURCE`; for AVCam, `launchctl setenv` in the sim). Load average 4–20 for the timings below, 33–94 for the first
plain S runs on 26.3.

| Stack | 17.5 S / R | 26.3 S / R | 27.0 S / R |
|---|---|---|---|
| plain AVF, webcam mode: back 10 s + photo + 5 s movie, front + photo + movie, 6 geometry cases | 8/8, 8/8 | 8/8, 8/8 | 8/8, 8/8 |
| plain AVF, its 14 checks | 14/14, 14/14 | 14/14, 14/14 | 14/14, 14/14 |
| VisionCamera 5 (`vc`) | 17/17, 17/17 | 17/17, 17/17 | 17/17, 17/17 |
| expo-camera | 7/8, 7/8 | 7/8, 7/8 | 7/8, 7/8 |
| Flutter `camera` + `mobile_scanner` | 17/18, 17/18 | 17/18, 17/18 | 17/18, 17/18 |
| AVCam (patched), 10 shutter presses | — (needs iOS 26) | 10/10, 10/10 | 10/10, 10/10 |

The only failures are the barcode checks (`expo.barcode.qr`, Flutter `scanner.qr+ean13`): they need the scene's QR and
EAN codes, which webcam frames don't carry. Webcam mode on every runtime and source: 30.0 fps (1080p60:
60.0 fps, each frame exactly twice), **0 dup / 0 skip**, photo and every movie frame stamped (150–151 unique of 150–151),
latency p50 11–16 ms S and 67–71 ms R, app CPU 6–14 %. Geometry (decoded stamp orientation/scale) as expected for every
case: portrait back 3/0.8889, 90° 420v 0/0.8889, landscapeRight 4K 0° 2/0.5, front 90° mirrored 4/0.8889, upsideDown
1440×1920 270° 0/0.6667, landscapeLeft 1080p60 180° 2/0.5. Preview: screenshots show the pattern, cropped to fill, on
the front and back. The R matrix ran on all three runtimes at once, so three simulators shared one helper. Front/back:
one camera feeds both positions; this Mac had no second ordinary camera (its others were a Continuity Camera and Desk
View cameras, not opened).

| Soak and lifecycle (27.0 unless noted) | Result |
|---|---|
| Soak: one app, R, 1810 s; 36 cycles of 45 s video data, a photo and a 5 s movie; webcam → scene → webcam 3×, helper stopped and restarted 3× | Cycles clear of an event: 30.0 fps, 0 dup, latency p50 68–70 ms, photo and movie stamped every time. Engine over the whole run: 51 001 ticks, 49 681 new frames, 16 dup, 30 skipped (about one per cycle, at its photo or movie), 0 torn, path gpu throughout. App footprint 51 MB at the start and the end (67 MB for a stretch in between; samples during the fixture's own movie decode read up to 475 MB); helper 179–215 MB with the camera open. Every event recovered without a relaunch |
| Leave/re-enter, 26.3, S: 100 × (4 s webcam, 4 s scene), one process | 100/100 live, 100 left, gpu each time, no crash; footprint after leaving 31 → 24 MB. Host IOSurface count 504 → 494 (range 484–504), unchanged when the app was then terminated; a control run (same process and schedule, scene only) 504 → 495 (487–504): no growth |
| Camera closed after the last reader | 10.0 s (S and R) |
| Idle quit with no reader | 600 s (S and R), ring cleared and unlinked |
| Stall scenarios with Core Image setup under the 250 ms bound (26.3, 27.0) | one stall recovers after ~605 ms; three in 30 s go to the CG canvas; a stall across leave/re-enter recovers with a new context; no crash |
| CLI | `--front`/`--back` naming two cameras refused (exit 64, nothing started); the same camera accepted; front camera through the CLI's control file 30 fps, 0 dup / 0 skip; `source scene` stops the helper |
