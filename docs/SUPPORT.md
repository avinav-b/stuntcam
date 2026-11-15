<!-- SPDX-License-Identifier: Apache-2.0 -->
# Support policy

stuntcam is open source (Apache-2.0) and maintained on a best-effort basis. This page says what has been verified, what
happens outside that, how to report a problem and what to expect back. Every claim here comes from the measured runs in
[MATRIX.md](MATRIX.md) and from the design ([design.md](design.md) §11–§12).

## What is supported

| | Verified | Notes |
|---|---|---|
| **Xcode** | 27.0 (build 27A266a) | The only Xcode measured so far. ⌘R, ⌘U, `xcodebuild test` (hosted and UI tests) and `stuntcam exec` were all run on it. A second Xcode side by side, and an Xcode update with a simulator booted, are not tested yet (design §11 U4 part 2) |
| **iOS Simulator runtimes, full** | 26.3.1 (23D8133), 27.0 (24A434) | Boot mode: the whole matrix (plain AVFoundation, VisionCamera 5, expo-camera, Flutter `camera` + `mobile_scanner`), orientation, Xcode Run and tests, soak, TSan torture, mutation suite. Launchd mode: the matrix, orientation and UI tests via `stuntcam exec` (hosted tests run there without a camera). MATRIX.md; measured internally, W4 e46 |
| **iOS Simulator runtime, point release** | 27.2 (24B8117) | W4 e56: the self-test (`verify` 5/5 pass); boot mode: the matrix (plain AVFoundation, VisionCamera 5, expo-camera, Flutter `camera` + `mobile_scanner`), plain-AVFoundation orientation, `conformance.sh` and a 20-launch plain soak; launchd mode: the matrix and plain orientation. Preflight passed on 27.0's layout with no engine change. Not run on 27.2: Xcode Run and tests, `stuntcam exec`/UI tests, `activation.sh`, the all-app soak, TSan, the mutation suite, the other stacks' orientation. measured internally, W4 e56 |
| **iOS Simulator runtime, smoke** | 17.5 (21F79) | A smaller run that passed in boot and launchd modes (e46): the self-test, the four app stacks' checks and plain-AVFoundation orientation. If an app can't open stuntcam's state folder, its records go to its container's `tmp/` instead (any runtime; `status` and `sentinel clear` look there) |
| **Mac** | Apple silicon | Every measurement ran on Apple-silicon Macs. The 26 and 27 simulator runtimes are arm64-only. Intel Macs are untested |
| **macOS** | 26.6 | The host of the measured runs; other versions untested |
| **Devices** | never | stuntcam is for the Simulator only. Nothing is linked into your app, and the libraries are built for the simulator platform, so device builds never load them |

**Camera stacks:** plain AVFoundation (Swift, Objective-C), VisionCamera 5, expo-camera, Flutter `camera` and
`mobile_scanner`. Known gaps: Expo Go is untested; Flutter `camera` in landscape is 90° off; Detox needs launchd mode
(docs/TROUBLESHOOTING.md). Libraries that compile their camera code out of simulator builds (for example VisionCamera 4) can't be fixed at
runtime by stuntcam; that is a question for the library (design §12, §13).

stuntcam 1.0 is on npm (`npx stuntcam up`). Until v1.1 there is no webcam source.

## Outside what's verified

stuntcam is built to fail safe: a runtime layout the preflight gate doesn't recognise gives your app the stock simulator (no
camera) with a line saying why. A semantic change can still slip past a shape check (design §13), which is why the sentinel
latches an app after two crashes attributed to stuntcam.

- **Another runtime** (18.x, a point release such as 26.5 or 27.1, a beta). Xcode 27.0's default (arm64) download request reports iOS 18 runtimes as not available (W4 e56; Apple's index lists 18.x as universal-only, and the universal variant wasn't tried), so 18.x is unmeasured. A device-specific runtime such as 27.2 `24B91016s` (`iOS-27-2-iPhone19-4`) has no device type Xcode 27.0 can create, and `verify` reports it `inconclusive` (W4 e56). Before arming, `stuntcam up` runs its
  self-test on that runtime. If the self-test fails, `up` doesn't arm (unless `--allow-unverified` or `--skip-verify`); if it
can't reach a verdict, `up` arms anyway with a warning. At the app's first camera call, a preflight check
  compares the runtime's private surface with what the engine expects; anything off means no camera, not a crash.
  "Inconclusive" (the Mac was too busy, or the simulator didn't boot in time) is never cached; see
  [TROUBLESHOOTING.md](TROUBLESHOOTING.md).
- **Another Xcode build.** Xcode Run and tests insert their own libraries, which boot mode displaces and the loader puts
  back. The libraries to restore are recorded per Xcode build in `restore-rules/<build>.json`; only `27A266a.json` exists.
  For a build without a file the loader uses the newest file it has, and every app it restores for prints:

  ```
  restore rules unverified for Xcode <build>; if console/debugger misbehave use --mode launchd
  ```

  `up` prints the same line, and `stuntcam doctor` says whether the rules for the Xcode in use are verified. "Unverified"
  means exactly this: the camera is expected to work (measured with a faked build id; a second real Xcode is untested), but the debugger, console or test injection under that Xcode have not been
  checked, and launchd mode (no Xcode Run camera, nothing of Xcode's displaced) is the safe choice. After switching or
  updating Xcode, run `stuntcam up` again from a shell where the new one is active; `doctor` flags a mismatch.

## Reporting a problem

Open an issue with the bug report template. It asks for:

- the output of `stuntcam doctor --json` and `stuntcam status --json`;
- the Xcode build (`xcodebuild -version`) and the simulator runtime;
- how the simulator was armed (boot or launchd mode, `stuntcam exec`), and how you launched the app (⌘R, ⌘U,
  `xcodebuild test`, `expo run:ios`, `flutter run`, …);
- the camera stack and its version, what you expected and what happened;
- `stuntcam sentinel ls` if an app is latched.

`doctor --json` is the triage path for "no camera" reports (design §13): a bug in stuntcam's safety machinery shows up as
a false "no camera", and the doctor output says which check stood in the way.

## What to expect

- **Best effort.** stuntcam has a single maintainer and no paid support. There is no response-time commitment.
- **Bugs are fixed once.** Every bug found gets an entry in [ledger.md](ledger.md) with the test that guards it.
- **Upstream problems go upstream.** If the report shows a library compiled its camera out of simulator
  builds, or an app's own bug (Apple's AVCam sample is one: see TROUBLESHOOTING.md), the fix belongs to that project.

## Yearly cadence

Apple ships a major iOS and Xcode each year, plus point releases. The plan (design §12 "Yearly maintenance"):

| When | What | Year-1 budget |
|---|---|---|
| June (betas) | Inventory the new runtime's private surface (`tools/inventory.py`) and diff Xcode's inserted libraries against the restore rules (that diff is not built yet); run the self-test | 3–5 days |
| Each major runtime | Port the engine: 26 → 27 took +34/−9 lines and one key; 17.5 → 26 needed one offset and one semantic change | 2–5 days |
| Point releases (6–8 a year) | The runtime inherits its major's entry and the local self-test decides; no code change expected (measured once: 27.0 → 27.2 needed none, W4 e56; design §11 U10) | about 1 day |
| Each Xcode build | A measured `restore-rules/<build>.json` | 1–2 days |
| Ongoing | Fixture updates for the five camera stacks | 7–10 days |
| Ongoing | Ledger and self-test upkeep, triage of reports | 6–8 days |

About 4–5½ weeks in the first year, 3–4 weeks after.

Until a port lands, the runtime or Xcode build is outside what's verified, and gets the behaviour described above.
