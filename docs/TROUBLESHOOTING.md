# Troubleshooting

Start with `stuntcam doctor`: it checks Xcode, the runtimes, the host directory, the build and every booted simulator.
Then `stuntcam status` shows what each simulator is doing. The common cases:

## No camera at all ("no devices", a black preview)

| Check | Fix |
|---|---|
| `status` says **booted-unarmed** | `stuntcam up` (or `up --reboot` for Xcode Run) |
| `status` says **launchd** and you used ⌘R | Xcode Run sets its own libraries and skips launchd mode: `stuntcam up --reboot` |
| `status` says the simulator was rebooted since `up` (Xcode, Simulator.app or a Mac restart booted it) | `stuntcam up --reboot` (plain `up` on a running simulator arms launchd mode, which ⌘R skips). The login helper (`stuntcam service install --only <udid>`, a preview) arms such boots in launchd mode for you and says when ⌘R needs `--reboot` |
| `status` lists your app as **latched** | stuntcam hung or crashed twice in it within its last 20 launches of this build, and keeps it on the stock simulator: `stuntcam sentinel ls` shows why, `stuntcam sentinel clear --bundle <id>` re-enables it (a new build or 24 hours also do). Please report what `sentinel ls` shows |
| `up` said **not arming: the canary failed** | stuntcam doesn't work on this runtime yet; `stuntcam verify --fresh` re-runs the self-test. Use another runtime, or `--allow-unverified` at your own risk |
| The app is launched by a tool that sets `DYLD_INSERT_LIBRARIES` itself (Detox, some test runners) | Boot mode breaks such launches: use launchd mode and give that tool `eval "$(stuntcam env)"`. A simulator booted armed can't switch while running: `xcrun simctl shutdown <UDID> && stuntcam up --mode launchd --udid <UDID>` |
| `stuntcam exec` ran your tests but says no app used the camera | Give the destination as `-destination 'platform=iOS Simulator,id=<UDID>'`: `name=…` destinations aren't armed |
| Another tool's library stopped loading after `up` | Boot mode displaces libraries a scheme or tool inserts: `stuntcam chain add <path to its dylib>` |

The stub logs why it loaded nothing for each launch; `stuntcam up --log` turns that log on (under
`~/Library/Application Support/stuntcam/logs/<udid>/`).

## Files in ~/Documents, ~/Desktop, ~/Downloads or iCloud Drive

Simulator processes can't read macOS privacy-protected folders, and nothing says so when a load fails there. stuntcam
copies its own files, and images or videos you pass to `--source` or `stuntcam source`, into
`~/Library/Application Support/stuntcam`, so a checkout in `~/Documents` is fine. Two things aren't copied: a library you
`chain add` (it is refused if it's in one of those folders: move it first) and the `stuntcam` binary the login helper
runs (`service install` refuses a protected location). `doctor` warns about protected paths.

## The first `stuntcam up` takes a minute

On a runtime's first use, `up` creates a hidden throwaway simulator and runs the self-test on it (about 30–90 s on a busy
Mac). The verdict is cached, so the next `up` on that runtime skips it. If the release's own self-test passed on your
runtime build, the package ships that verdict and `up` skips the self-test from the start (`stuntcam verify` says
"shipped with this package"). To arm without it, `stuntcam up --skip-verify`: if stuntcam doesn't work on that runtime,
your app then gets no camera (or a broken one) instead of a refused `up`.

## The self-test says inconclusive

The Mac was too busy for the self-test's timing checks, or the simulator didn't boot in time. It is never cached; `up`
arms with a warning and the next `up` tries again. If it happens every time, run `stuntcam verify --fresh` when the Mac is
quieter.

## Video is slower than the format says

4K at 60 fps records at about 16–32 fps: the simulator encodes in software. Lower formats keep their frame rate, though on a heavily loaded Mac 4K30 and 1080p60 can drop a few frames. A
hardware-encoded path is planned for v1.1.

## The webcam

`stuntcam source webcam` starts a small helper app, "Stuntcam Camera", that feeds your Mac's camera to the simulator.

| What you see | Fix |
|---|---|
| `source webcam` says camera access is denied, or the picture says **webcam offline: helper denied** | Turn on Stuntcam Camera in System Settings › Privacy & Security › Camera, then run `stuntcam source webcam` again. Over ssh or in CI nobody can answer the prompt: use `video:` there |
| **webcam offline: helper not running** | The helper isn't running: it quits on `stuntcam down`, on switching to another source, and after 10 minutes unused. Run `stuntcam source webcam` again; running apps pick it up within a second, no relaunch needed |
| **webcam offline: no fresh frames from the helper** | The camera stopped sending: it was unplugged, or another app took it over. Plug it back in, or quit the other app (or stop its video); if the picture doesn't come back, run `stuntcam source webcam` again |
| **update stuntcam (…)** | The running camera helper and this stuntcam are different versions (two installs, or an upgrade mid-session). Run `stuntcam source webcam` from the stuntcam you use: it replaces the old helper. If an app shows it, the app runs an older stuntcam: `stuntcam up`, then relaunch the app |

## A feature your app needs isn't there

Not supported yet: a 720p format, Live Photo and depth data: your app sees them as unavailable, as on a device without
them. Zoom (`videoZoomFactor`, and on iOS 27 `rampToVideoZoomFactor`) is accepted but the picture never
zooms; on iOS 26 a ramp is ignored (the factor doesn't move). Focus can't be locked. Expo Go and Flutter's `camera` plugin in landscape are known
gaps.

## Apple's AVCam sample crashes when you take a photo

Apple's AVCam sample crashes at the shutter under stuntcam: it saves a Live Photo video even when Live Photo is
unsupported, and AVFoundation throws (`livePhotoMovieFileURL must be nil if self.livePhotoCaptureEnabled is NO`). A real
iPhone supports Live Photo, so the sample never gets there. Turn off its Live Photo toggle (the Live Photo icon above the
preview), or build it from [fixtures/avcam](../fixtures/avcam/README.md): a one-line fix, plus a small patch so the current
sample builds with Xcode 27.0.

## UI tests with Address Sanitizer hang in boot mode

You see `stuntcam boot mode displaced AddressSanitizer`, then the UI-test runner aborts with "Interceptors are not
working" and `xcodebuild test` hangs. Xcode gives the UI-test runner its sanitizer runtime only through its per-launch
library list, and boot mode replaces that list; a sanitizer runtime can't be loaded after launch. Hosted (unit) tests
with sanitizers, ⌘R with sanitizer schemes and plain UI tests are not affected. Thread Sanitizer UI tests are expected to
behave the same (not measured yet).

No arming mode gives a sanitizer UI test the camera today (W4 e32): in launchd mode, and on a simulator stuntcam hasn't
armed, those tests run and pass, but the app under test has no camera, because Xcode sets its own library list for every
process it launches in a test. So run sanitizer UI tests on a simulator stuntcam hasn't boot-armed (shut it down with
`xcrun simctl shutdown <udid>` and boot it normally, or use another simulator), and keep camera UI tests to runs without
sanitizers.

## Turning stuntcam off

- One launch: `SIMCAM=0` in the app's environment (an Xcode scheme variable, or `SIMCTL_CHILD_SIMCAM=0` for
  `xcrun simctl launch`).
- A simulator: `stuntcam down`; running apps keep the camera until they quit.
- The login helper: `stuntcam service uninstall`.

## Reporting a problem

Include `stuntcam doctor --json` and `stuntcam status --json` output, the iOS runtime and Xcode build, the camera library and its
version, and `stuntcam sentinel ls` if an app is latched. The issue tracker's bug report template asks for all of these; what's
verified and what to expect back: [SUPPORT.md](SUPPORT.md).
