# stuntcam

A camera for the iOS Simulator. Your app's real camera code runs unmodified: no code, dependency or project changes. The
simulator shows a synthetic scene (or your image or video) through Apple's own AVFoundation, so previews, photos, video,
QR and barcode scanning and orientation behave the way they do on a phone.

> **Status: pre-release.** The engine, tests and `stuntcam` command are here; packages aren't published yet: build from
> source (below). Physical devices are never involved.

Two things to know first:

- **Run `stuntcam up` before ⌘R.** Xcode Run gets the camera only on a simulator that `up` booted. If Xcode, Simulator
  or a Mac restart has booted it since, run `stuntcam up --reboot` (details under [Xcode](#xcode-swift-objective-c)).
- **No webcam until v1.1.** The picture is a synthetic scene with a QR code and a barcode, or your image or video file.

## How it works

Like the [Pepper's ghost](https://en.wikipedia.org/wiki/Pepper%27s_ghost) stage illusion, the camera isn't really there.
stuntcam works out of sight underneath, and Apple's real AVFoundation does the performing on top.

Other simulator camera tools replace AVFoundation's objects with fakes, and apps crash when they touch a detail the fake
missed. stuntcam answers one layer lower, so apps (and VisionCamera, expo-camera, Flutter `camera`, barcode scanners)
only ever talk to Apple's genuine device, session, input and output objects. More in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Tested on iOS 17.5, 26.3 and 27.0 with Xcode 27.0 ([docs/MATRIX.md](docs/MATRIX.md)): a plain AVFoundation app,
VisionCamera 5.2.4, expo-camera 58 and Flutter `camera` + `mobile_scanner`.

## Install (pre-release: from source)

npm (`npx stuntcam up`) and Homebrew packages are built and tested locally (`scripts/package.sh`)
but not published yet. Until then:

Needs a Mac with Xcode 27, and Go 1.23 or newer.

```sh
git clone https://github.com/avinav-b/stuntcam.git && cd stuntcam
./build.sh                          # engine, loader, stub (ad-hoc signed)
scripts/build-fixtures.sh canary    # the self-test app (its "no SIMCAM_UDID_* set" note is for the test scripts; ignore it)
cli/build.sh                        # the stuntcam command → cli/bin/stuntcam
export PATH="$PWD/cli/bin:$PATH"    # e.g. in your shell profile
```

`stuntcam` then works from any directory: it finds the build in the checkout it was built in. Run from inside another
stuntcam checkout, it uses that checkout's build instead. The checkout can live
anywhere, `~/Documents` included: stuntcam copies what simulators read into `~/Library/Application Support/stuntcam`,
because simulator processes can't read macOS privacy-protected folders.

## Quick start

```sh
stuntcam up          # self-test, then arm a simulator (boots it with the camera if it's shut down)
# then run your app as usual
stuntcam status      # what each simulator is doing
stuntcam doctor      # why there's no camera, if there isn't
stuntcam down        # stop: new launches get no camera, no reboot
```

`up` picks `--udid U`, else the only booted simulator, else the one booted most recently, and says which; with several
booted it asks for `--udid`. Its self-test
runs on a hidden throwaway simulator (about a minute the first time on a runtime, then cached). If the self-test fails on
your runtime, `up` doesn't arm, so your app never sees a half-working camera.

Your app still asks for camera permission as on a device (it needs `NSCameraUsageDescription`); `up --grant <bundle id>`
grants camera and microphone up front (it quits the app if it's running).

### Xcode (Swift, Objective-C)

Run `stuntcam up --udid <the simulator you run on>` **before** ⌘R. Xcode Run and Xcode tests insert their own libraries,
so the camera has to be in the simulator's boot environment: `up` boots a shut-down simulator that way. If the simulator
is already running, `up` arms it without a reboot, which covers launches from the command line but not ⌘R;
`stuntcam up --reboot` covers both. If Xcode or Simulator reboots it later, run `up --reboot` again. ⌘R and ⌘U are
verified on Xcode 27.0 (27A266a).

### React Native / Expo (VisionCamera, expo-camera)

```sh
stuntcam up
npx expo run:ios            # or: npx react-native run-ios
```

No plugin or config change. Expo Go is untested. Detox launches apps with its own libraries, so it needs launchd mode
plus `eval "$(stuntcam env)"` in its environment. A simulator `up` booted armed is in boot mode and stays so; switch it
with `xcrun simctl shutdown <UDID> && stuntcam up --mode launchd --udid <UDID>`.

### Flutter (`camera`, `mobile_scanner`)

```sh
stuntcam up
flutter run
```

Portrait is fully checked; landscape in Flutter's `camera` plugin is a known gap.

### Tests and CI (`xcodebuild test`)

```sh
stuntcam up --udid <UDID> --grant <your app's bundle id>     # once, to grant the camera
stuntcam exec -- xcodebuild test -scheme MyApp -destination 'platform=iOS Simulator,id=<UDID>'
```

`exec` boots a shut-down `-destination id=…` simulator armed before the command runs (xcodebuild's own boot would not
be), passes the command's exit code through, and reports which apps used the camera. Destinations given by `name=` are
not armed; use `id=`. On GitHub-hosted macOS runners the engine and its self-test have run with the test scripts; the
`stuntcam` command hasn't been tried there yet.

## What the camera does

- **Picture:** a synthetic scene with a QR code and an EAN-13 barcode, an image, or a video file
  (`up --source image:/path.png`, `video:/path.mov`). The default profile has the cameras of a Pro iPhone: back wide,
  ultra-wide, telephoto, dual wide, triple and LiDAR (as a device; no depth data), and front and TrueDepth.
- **Photos:** JPEG and HEIC with real EXIF, at the format's sizes, bursts, preview images.
- **Video:** `.mov` at the active format's size and frame rate, with AAC audio.
- **Audio:** a generated tone by default, so the Mac's microphone and its permission are never involved. The tone is
  recorded even when your app's microphone permission is "not determined" (a known difference from a device).
- **Scanning:** QR and EAN-13 through AVFoundation's metadata output and Vision.
- **Device behaviour:** orientation and rotation, front-camera preview mirroring, torch, focus and exposure points,
  and the interruption apps get when they go to the background.

**Known limits:** 4K60 video records at about 16–32 fps (the simulator's software encoder; 1080p60 is fine); no 720p
format yet; zoom is accepted but the picture doesn't zoom (on iOS 26 ramps are ignored); front photos keep the same EXIF orientation with mirroring
on; focus can't be locked; flash is offered but never fires; Live Photo, depth data, bracketed capture and MultiCam are
reported as unsupported; the back LiDAR camera is listed (as on an iPhone 17 Pro) but works as the wide camera with no
depth; no live webcam until v1.1.

> **Apple's AVCam sample crashes at the shutter** under stuntcam: it saves a Live Photo video even when Live Photo is
> unsupported, and AVFoundation throws. Turn off its Live Photo toggle, or build it from
> [fixtures/avcam](fixtures/avcam/README.md) (a one-line fix).

## Commands

| Command | Does |
|---|---|
| `up` | Self-test, then arm a simulator (boot mode if shut down or `--reboot`, launchd mode if running) |
| `down` | New launches stop loading stuntcam; no reboot |
| `status` | Every simulator: mode, build, latched apps |
| `doctor` | Checks Xcode, runtimes, the host directory, the build and each booted simulator |
| `verify` | The self-test on its own (`--fresh` to re-run it) |
| `exec -- CMD` | Run a command so simulators it boots come up armed |
| `env` | `export SIMCTL_CHILD_…` lines for tools that launch apps themselves |
| `source` | Switch the picture live: `scene`, `image:PATH`, `video:PATH` (running apps and the next ones) |
| `orient` | Set the virtual device orientation live (`portrait`, `landscapeLeft`, …, `auto`) |
| `swap` | Point an armed simulator at another build, from the next launch |
| `chain` | Extra libraries every app launch loads (another tool's loader) |
| `sentinel` | Crash/hang records and latches per app; `sentinel clear` re-enables an app |
| `service` | Preview: a login helper that arms simulators Xcode or Simulator boot, and tells you when Xcode Run needs `up --reboot` (`--only <udid>` or `--all`) |
| `gc` | Remove unused builds and unpacked self-tests, old self-test devices and state of deleted simulators |

Details and flags, including `trace` (what happened in one launch): [cli/README.md](cli/README.md).

## Safety

stuntcam only touches simulators you point it at, and steps aside rather than crash your app:

- Nothing is armed until your app's first camera call; apps that never use the camera are never armed. (A small
  orientation helper does run in every app on an armed simulator.)
- A preflight check at that first call: if the runtime isn't exactly what stuntcam knows, your app gets the stock
  simulator (no camera), never a crash.
- If a stuntcam operation hangs twice in a launch, that feature fails cleanly (an error, not a hang) for the rest of the
  launch. Two hangs or crashes in an app within its last 20 launches (and 24 hours) of a build keep that app on the
  stock simulator, until they age out, a new build arrives or you run `stuntcam sentinel clear`. Crashes while a
  debugger is attached never count.
- To turn it off for one launch, set `SIMCAM=0` in the app's environment (an Xcode scheme variable, or
  `SIMCTL_CHILD_SIMCAM=0` for `xcrun simctl launch`).

## Developing

`scripts/` boot a simulator with the engine and run the test apps (see each script's header). The test apps are in
`fixtures/`, the self-test in `canary/`, every known bug and what guards it in [docs/ledger.md](docs/ledger.md), and the
measured results in [docs/MATRIX.md](docs/MATRIX.md). Design: [docs/design.md](docs/design.md). Troubleshooting:
[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md). What's supported and how to report a bug:
[docs/SUPPORT.md](docs/SUPPORT.md).

## License

Apache-2.0. Third-party files in the tree (Expo and Flutter template files in the fixtures, Apple's lines in the AVCam
patch) are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
