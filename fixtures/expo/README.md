# fixtures/expo — Expo app with VisionCamera and expo-camera

An Expo (SDK 58, React Native 0.88) app that auto-runs a camera compatibility probe when it launches. It came from spike
m1b (copied in m5): `react-native-vision-camera` ^5.2.4 and `expo-camera` ~58.0.14. Results are printed with an `M1B|`
prefix and mirrored to `Documents/m1b.log` in the app container. Bundle id `dev.spike.m1bcam`.

Locked versions (`package-lock.json`; checked 2026-11-12 with `npm view`, both camera libraries are the latest stable, so
no upgrade was needed): react-native-vision-camera 5.2.4, expo-camera 58.0.14, expo 58.0.23, react-native 0.88.1,
react 19.3.0, react-native-nitro-modules 0.37.2, react-native-nitro-image 0.15.2, expo-file-system 58.0.13.

## Install dependencies and build

```sh
npm ci
(cd ios && LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 pod install)
xcodebuild -workspace ios/m1bcam.xcworkspace -scheme m1bcam -configuration Debug \
  -sdk iphonesimulator -derivedDataPath build/dd build      # e.g.; or `npx expo run:ios`
```

`ios/` is the native project the spikes built, kept in git together with `Podfile.lock`. To regenerate it from
`app.json`, run `npx expo prebuild -p ios --clean`.

## How the spikes launched it

Start Metro with `scripts/metro.sh start` (this app on :8099, or `$EXPO_PORT`). Then run `scripts/run.sh vc|expo <rt> boot|launch <tag>`.
It launches the app with:

```
-CAMTEST vc|expo -RCT_jsLocation localhost:8099
```

`-CAMTEST vc` tests VisionCamera and `-CAMTEST expo` tests expo-camera. `run.sh`'s `vc-audio` / `expo-audio` modes also
pass `-AUDIO 1` (record with audio, ask for microphone permission).

`App.tsx` is m12's copy, which adds more `-CAMTEST` modes:

- `vcorient` / `expoorient`: m12's orientation test (preview screenshot, photo, movie), driven by `scripts/lib.sh` and
  `scripts/orient-lib.sh`. Extra launch args: `-CAM back|front`, `-VCORIENT <orientationSource>` (VisionCamera, default
  `device`), `-RESPONSIVE 1` (expo-camera `responsiveOrientationWhenOrientationLocked`).
- `vccycle` / `expocycle`: open/close the camera `-CYCLES <n>` times (default 20).

## `-CAMTEST vcfull`: exhaustive VisionCamera checks

A generic check of a typical VisionCamera 5 camera screen, written from the library's TypeScript types. Run it with
`scripts/run.sh vcfull <rt> <inj> <tag>` (or `vcfull-audio`, which adds `-AUDIO 1`); it waits up to 300 s (`MAXW`).
The log starts with `BEGIN vcfull` and ends with `INFO vcf.summary pass=<n> fail=<m> known=<k> unexpected=<u> total=<t>
expected_total=<T> na=<a> [<unexpected names>]` and `END vcfull`. `fail` counts every `FAIL` line, including the
expected ones; `known` is how many of them are pre-registered; `na` counts `n/a` passes. `run.sh` prints `pass`/`fail`
(counting the log's lines) plus the summary's known/unexpected/total/expected_total/na. A green run reads exactly
`known=4 na=3 unexpected=0 total=102` (`total=109` with audio).

**Denominator: 102 checks** (109 with `-AUDIO 1`). **Expected result on the engine as of week 1: 98/102** (105/109 with
audio), pre-registered before the final week-1 run (e14). Exactly these four failures (`VCF_EXPECTED` in `App.tsx`):
1. `vcf.video.4k60.fps`: the simulator's software encoder manages 16–32 fps at 4K60 (v1.0 cap).
2. `vcf.format.video.1280x720`: the engine's profiles offer no 720p format (engine gap, TODO.md).
3. `vcf.zoom.ramp`: a zoom ramp (VisionCamera's `startZoomAnimation`) never reaches the engine and times out (engine gap).
4. `vcf.front.mirror`: front photos with mirroring on and off don't differ (EXIF Orientation 6 both times; engine gap).
And exactly these three checks pass as `n/a` (`VCF_EXPECTED_NA`): `vcf.format.video.1280x720.fps30`, `…fps60` (no 720p
format) and `vcf.focus.locked` (the engine offers no focus locking; engine gap).
The app's summary line reports `unexpected=<n>`: any other failure, any other n/a, a `vcf.aborted` (a call outside a step
threw) or a total other than 102/109 is counted there, and must be 0. These expectations are for 26.3 and 27.0; on 17.5
`vcfull` is informational. When the engine gaps are fixed the expectation becomes 101/102.

| Group | Checks | n |
|---|---|---|
| Setup | `vcf.permission`; `vcf.micPermission` (audio only); `vcf.camera.started` (back camera `onStarted` within 15 s) | 2 (+1) |
| Device | `vcf.device.back` (back camera with the most constituents); `.triple` (it is the Triple camera if the profile offers one); `.constituents` (virtual, ≥ 2 constituents; a Triple has ultra-wide, wide and telephoto); `.zoomSwitchFactors` (non-empty); `.zoomRange` (`minZoom` < `maxZoom`) | 5 |
| Formats | `vcf.format.video.<WxH>` for 3840x2160, 1920x1080, 1280x720 (in `getSupportedResolutions('video')`); `vcf.format.video.<WxH>.fps30` / `.fps60` (`resolveConstraints` with that resolution first, then the fps, selects that fps); `vcf.format.photo.max` (largest 4:3 photo resolution is 4032x3024) | 10 |
| Depth off | `vcf.depth.unsupported` (`mediaTypes` has no `depth`); `vcf.depth.noError` (no `onError` from mounting with default options) | 2 |
| Controls | `vcf.zoom.set2` (set, read back); `vcf.zoom.ramp` (`startZoomAnimation` to 4 and back to 2, each ≤ 4 s, read back); `vcf.focus.point` (`focusTo` resolves ≤ 2 s); `vcf.focus.locked` (`focusTo` with `adaptiveness: 'locked'` resolves ≤ 2 s); `vcf.exposure.bias` (+1 resolves ≤ 1 s, reads back); `vcf.torch.on`, `vcf.torch.off` (read back) | 7 |
| Photos | after a 5 s wait from `onStarted`, for each of 8 photos `vcf.photo.<id>.delivered`, `.bytes` (JPEG, or HEIC where requested), `.dims` (reported and encoded size = the max photo resolution), `.exif` (EXIF Orientation 6: back camera, portrait), `.timestamp` (within 2 s of the shutter), `.path` (unique). Ids: `off`, `on`, `auto` (spaced, flash modes), `burst1`–`burst3` (fired back-to-back, no awaits), `speed` (`qualityPrioritization: 'speed'`, quality 0.5), `quality` (`'quality'`, quality 1, HEIC) | 48 |
| Video (3 s each) | for `1080p30` (muted), `1080p60`, `4k30`, `4k60`: `vcf.video.<id>.finished`, `.file`, `.duration` (3 ± 0.4 s), `.dims` (coded size = the format), `.fps` (frames / track duration ≥ 90% of target); `vcf.video.torch.on` / `.off` during the 1080p30 recording. With `-AUDIO 1` also `1080p30a` (with audio): the same 5 plus `.audio` (has a sound track) | 22 (+6) |
| Front | `vcf.front.mirrorOn` / `.mirrorOff`: `.delivered`, `.dims` (= front camera's max photo resolution); `vcf.front.mirror` (the two differ in `isMirrored` or EXIF Orientation, as on a device) | 5 |
| End | `vcf.errors.none` (no `onError` in any stage) | 1 |

Rules: a check that does not apply passes with an `n/a:` note (an fps check whose resolution is not listed or whose fps
the device doesn't offer; `.focus.locked` when `supportsFocusLocking` is false). `.triple` without a Triple camera fails.
`speed` falls back to `'balanced'` when `supportsSpeedQualityPrioritization` is false. Photo timestamps are on the host
clock (seconds since boot); the shutter time is RN's `performance.now()`, the same clock. Media checks read the files in
the app (JPEG markers and EXIF, HEIC `ispe`, the movie's `moov` boxes); `run.sh` also runs `tools/bin/avinfo` on each
`path=` into `media.txt`. The preview/thumbnail image (`onPreviewImageAvailable`) is recorded, not checked:
`INFO vcf.photo.previewImage delivered=<n>`.

The burst uses `capturePhoto()` + `saveToTemporaryFileAsync()`, the other photos `capturePhotoToFile()`. VisionCamera
5.2.4's `capturePhotoToFile()` creates its `AVCapturePhotoSettings` in a Swift `Task`, so un-awaited calls can reach
`-capturePhotoWithSettings:delegate:` out of `uniqueID` order; AVFoundation then throws "Settings may not be re-used"
and the app traps (a library bug that would hit a device too).

Status 2026-11-12, before `vcf.front.mirror` and the Orientation-6 check existed (engine at `b67b94f`, iOS 26.3,
`cfg-m10.plist`): 99/102, two failures beyond the known one, both engine gaps: `vcf.format.video.1280x720` (no 720p format in the profile) and `vcf.zoom.ramp` (the engine does not act on
`rampToVideoZoomFactor:withRate:`, so the ramp never starts or ends).

## `-CAMTEST vclidar`: the back LiDAR depth device (w4 e62)
Picks the back device with `type === 'lidar-depth'` (and logs what `useCameraDevice('back', { physicalDevices:
['lidar-depth'] })` returns), mounts `<Camera>` with a photo output, takes a photo, asks for one with `enableDepthData:
true`, then remounts with a depth frame output added and takes another photo. `-LIDARDEV wide` runs the same steps on the
back wide camera (control). run.sh has no case for it; drive it like the other `-CAMTEST` modes
(`EXPO_PORT` = your Metro).
