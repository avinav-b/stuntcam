# AVCam (Apple's sample camera app) under stuntcam

[AVCam: Building a camera app](https://developer.apple.com/documentation/avfoundation/avcam-building-a-camera-app) is
Apple's sample camera app (SwiftUI, AVFoundation): preview, photo and video capture, Live Photo, front/back switch,
saving to Photos. As Apple ships it, it crashes at the first shutter press under stuntcam. This directory holds a small
patch and a build script, not Apple's code: `build.sh` downloads the sample (or takes your copy) and patches it.

Licence: the sample is Apple's, under the `LICENSE.txt` in its zip. In `avcam.patch`, the context and removed lines are
Apple's code under that licence; only the added lines and this directory's scripts are this repo's (Apache-2.0).

## Version

| | |
|---|---|
| Sample | "AVCam: Building a camera app", developer.apple.com/documentation/avfoundation/avcam-building-a-camera-app |
| What the page shows | no version number or date; requirements iOS 27.0+, iPadOS 27.0+, Mac Catalyst 27.0+, Xcode 27.0+ |
| Download | `https://docs-assets.developer.apple.com/published/cd906d1738b2/AVCamBuildingACameraApp.zip` (the page's Download button; no login or browser needed). Server date 2026-11-04 |
| Zip sha256 | `5a1981f90bb30827121324d4b33198d58de9b9d68e3508b64f0aabc18bbc0e53` (19,771,840 bytes; same zip on 2026-11-11 and 2026-11-12) |
| Inside | the sample's own git history; last commit `0abcc86` (2026-10-23) "Updated for latest OS release." |

`build.sh` warns if your zip has another checksum and tries anyway; if Apple updates the sample, the patch may no longer
apply.

## Why it crashes under stuntcam

stuntcam reports Live Photo as unsupported (`isLivePhotoCaptureSupported` is NO), as a device without Live Photo would.
AVCam turns Live Photo on only where it's supported, but its Live Photo button (on by default) decides alone whether each
photo asks for a Live Photo movie: it sets `livePhotoMovieFileURL` without checking that Live Photo is enabled. With Live
Photo off on the photo output, AVFoundation throws `NSInvalidArgumentException: livePhotoMovieFileURL must be nil if
self.livePhotoCaptureEnabled is NO` inside `capturePhoto`, and the uncaught exception ends the app (SIGABRT). A real
iPhone supports Live Photo, so the sample never reaches that path there. (Measured in
measured internally, W4 e22; this fixture: measured internally, W4 e23a.)

Without the patch, turning AVCam's Live Photo button off (the Live Photo icon above the preview, until it shows a slash)
also avoids the crash.

## The patch (`avcam.patch`)

Two parts, each commented in the diff:

**(a) The Live Photo fix**, `AVCam/Capture/PhotoCapture.swift`, one line:

```swift
photoSettings.livePhotoMovieFileURL = features.isLivePhotoEnabled && photoOutput.isLivePhotoCaptureEnabled ? URL.movieFileURL : nil
```

The movie URL is set only when Live Photo capture is enabled on the photo output. AVCam enables it wherever it's
supported (`isLivePhotoCaptureEnabled = isLivePhotoCaptureSupported` in `updateConfiguration`), so on a device with Live
Photo nothing changes. Where it's unsupported the app takes ordinary photos. The Live Photo button still shows "on" (it
is AVCam's preference, not the output's state); no LIVE badge appears, as AVCam shows it only for real Live Photos.

**(b) Building with Xcode 27.0**, `AVCam/CameraView.swift`, three hunks. The current sample uses SwiftUI's
`toolbarVerticalEdge` environment value, which is new in the iOS 27.1 SDK. `@available(iOS 27.1, *)` doesn't help when
the SDK doesn't have the symbol, so with Xcode 27.0 the file doesn't compile. The hunks:
1. `#if AVCAM_SDK_27_1` before the `ToolbarEdgeSafeArea` modifier (the only user of `toolbarVerticalEdge`);
2. `#endif` after it;
3. in `ignoresSafeAreaAtToolbarEdge()`, the original `if #available(iOS 27.1, *)` block inside `#if AVCAM_SDK_27_1`, and
   `self` otherwise, which is what the sample itself does before iOS 27.1 (views stay inside the safe area at the
   toolbar edge).

`build.sh` sets `AVCAM_SDK_27_1` when the Simulator SDK is 27.1 or later, so there the sample's code is compiled
unchanged. If you apply the patch by hand and build in Xcode with SDK 27.1 or later, add `AVCAM_SDK_27_1` to the target's
Active Compilation Conditions, or the toolbar-edge behaviour is silently dropped. (Spike m12's version of this fix deleted the modifier: 43 lines; this one adds 8 and removes none.)

Not in the patch, passed to `xcodebuild` by `build.sh` instead: `IPHONEOS_DEPLOYMENT_TARGET=26.0` (the sample says 27.0;
it builds as is for 26.0, so it also installs on iOS 26.x simulators) and `CODE_SIGNING_ALLOWED=NO`.

Left as Apple wrote it: in the Simulator, AVCam puts a static picture over its preview (`#if
targetEnvironment(simulator)` in `CameraPreview.swift`), so the preview shows Apple's street photo, not stuntcam's
picture. The photos it takes are stuntcam's picture (HEIC, 4032×3024).

## Build and run

```sh
fixtures/avcam/build.sh                       # downloads the zip to fixtures/avcam/build/AVCam.zip, or:
fixtures/avcam/build.sh ~/Downloads/AVCamBuildingACameraApp.zip    # a zip or an unzipped sample folder
```

It copies the sample to `fixtures/avcam/build/patched/src/` (git-ignored), applies the patch, builds with `xcodebuild`
(Debug, iOS Simulator, this Mac's architecture) and prints the app path:
`fixtures/avcam/build/patched/dd/Build/Products/Debug-iphonesimulator/AVCam.app`, bundle id
`com.example.apple-samplecode.AVCam`. `--no-livephoto-fix` builds with part (b) only (the sample's crash, for
comparison) into `build/control/`.

Then, on a simulator armed with stuntcam (`stuntcam up --udid <UDID>`, or `scripts/boot.sh <rt> stub` from the repo):

```sh
xcrun simctl install <UDID> fixtures/avcam/build/patched/dd/Build/Products/Debug-iphonesimulator/AVCam.app
for s in camera microphone photos-add location; do xcrun simctl privacy <UDID> grant $s com.example.apple-samplecode.AVCam; done
xcrun simctl launch <UDID> com.example.apple-samplecode.AVCam
```

AVCam asks for the camera at start, and for Photos (add only) and location at the first save; the grants skip those
alerts. Photos land in the simulator's Photos library.

## The test (`test.sh`, `uitest/`)

`uitest/` is a standalone UI-test project (test-only; it isn't added to the sample). Its one test launches the installed
AVCam, waits for the capture session, reads the Live Photo button, and taps the shutter 20 times, 2 s apart, checking after
each tap that AVCam is still in the foreground. Build it once:

```sh
cd fixtures/avcam/uitest && xcodebuild -project AVCamShutterUITests.xcodeproj -scheme AVCamShutterUITests \
  -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' -derivedDataPath ../build/uitest-dd \
  ARCHS=$(uname -m) CODE_SIGNING_ALLOWED=NO build-for-testing
```

then `fixtures/avcam/test.sh <rt> patched|control <tag> [shots]` (sim booted by `scripts/boot.sh <rt> stub`, UDID in
`$SIMCAM_UDID_<rt>`): fresh install, grants, `xcodebuild test-without-building`, then counts the new files in the
simulator's DCIM folder and runs `tools/crashes.py` over the run. Results go to `$SIMCAM_HOME/results/avcam-<rt>-<variant>-<tag>/`.
