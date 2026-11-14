# features-probe (w4 e22)

A measurement app, not a test. It reads which camera features AVFoundation says the camera supports, and records what
happens when an app turns them on. It logs and checks nothing. Results: measured internally, W4 e22.
`scripts/features.sh <rt>` checks its read-out and `ifsupported` launch for the features design §6.5 wants unsupported
(bracket, MultiCam device sets, flash; ledger L53–L55, measured internally, W4 e23b).

Every line is `PROBE| <CFAbsoluteTime> <s since launch> <text>` (CFAbsoluteTime is the engine log's clock).

**Read-out** (every launch). For every camera a `DiscoverySession` of all 8 built-in types returns, it logs:
- the device's getters (`R <camera> device <key> <value>`), every format with the format keys that read YES (`FMT`), and
  per key how many formats read YES (`FMTCOUNT <camera> <key> <n>/<total>`). Derived keys: HLG / Apple Log in
  `supportedColorSpaces`, `supportedDepthDataFormats` non-empty, max frame rate > 30 and > 60;
- configuration 1 (preset photo, `AVCapturePhotoOutput` + `AVCaptureVideoDataOutput`): every photo-output key, the video
  data output and the input. It runs 1 s, then logs any photo key that changed (`photo-run`);
- configuration 2 (preset high, `AVCaptureMovieFileOutput` + video data output): the movie-output keys;
- class-level keys (`AVCaptureDevice.isCenterStageEnabled` & co.), `AVCaptureMultiCamSession.isMultiCamSupported` and
  `supportedMultiCamDeviceSets`. With no camera at all, the keys of bare outputs (camera `none`).

Keys are read with KVC after `respondsToSelector:`, so a key this OS doesn't have reads `absent`, not a crash.

**Feature launches** (`-mode feature -feature <f> -camera <label>`). The probe does the read-out of that camera, then
turns `<f>` on the way Apple's AVCam does, then takes 5 photos or one 3 s movie. Photos are taken one at a time, and the
probe waits up to 10 s for `didFinishCapture` (`HANG` if it doesn't come). It logs every `SET` (ok or the exception),
every delegate callback with its error (`CB`), what each photo or movie contains, and one `RESULT` line. Objective-C
exceptions are caught by `SCTry.m`. `-nocatch 1` calls `capturePhoto` without the catch, so the exception goes uncaught
as it would in an app.

| feature | what it turns on |
|---|---|
| `baseline`, `moviebaseline` | nothing (controls) |
| `livephoto` | `isLivePhotoCaptureEnabled = true`; per shot `livePhotoMovieFileURL` only if that took |
| `avcam` | AVCam's own `updateConfiguration` + `createPhotoSettings` with its UI defaults (Live Photo toggle on, quality) |
| `depth`, `portraitmatte`, `semantic`, `calibration` | depth on the output; settings take the output's value (AVCam) |
| `proraw`, `raw` | `isAppleProRAWEnabled`; RAW settings with the first ProRAW / Bayer format |
| `zsl`, `responsive`, `fastprio`, `deferred`, `quality`, `constantcolor`, `constituents`, `contentaware` | the property of that name |
| `shuttersound`, `flash`, `bracket`, `maxres` | `isShutterSoundSuppressionEnabled`; `flashMode = .on`; 3-shot AE bracket −2/0/+2 EV; largest `maxPhotoDimensions` |
| `videohdr`, `hlg`, `applelog`, `fps`, `spatial`, `cinematic` | a format that reads YES, then the device/output property; `fps`: the highest frame rate |
| `centerstage`, `reactions`, `portraiteffect`, `studiolight` | the app-settable part; logs the user-controlled state |
| `multicam` | `AVCaptureMultiCamSession`, back + front wide, formats with `isMultiCamSupported` if there are any |
| `ifsupported` (e23b) | AVCam-style "enable if supported": flash `.auto`/`.on` if offered, a 3-shot bracket if `maxBracketedCapturePhotoCount` ≥ 3, MultiCam if back-wide + front-wide is a supported device set; RESULT has `enabled=` (none = nothing turned on) |
| `fps1080` (e23b) | 1920×1080 at 60 fps (min/max frame duration 1/60); with `MOVIE_SECONDS=10` a 10 s movie |

```sh
SIMCAM_UDID_27=… SIMCAM_UDID_263=… fixtures/features-probe/build.sh     # build/featuresprobe.app (dev.simcam.e22.featuresprobe), installs
fixtures/features-probe/drive.sh <rt> boot|none <tag> read               # one read-out launch
NOCATCH=1 fixtures/features-probe/drive.sh <rt> boot <tag> feature <f> [camera]   # one feature launch (camera default back-wide)
XENV="M15_OLD_BRACKET=1" MOVIE_SECONDS=10 fixtures/features-probe/drive.sh …   # per-launch engine settings; movie length (e23b)
fixtures/features-probe/matrix.sh <rt> <tag>                             # e22's armed runs on one runtime
python3 -I fixtures/features-probe/summarize.py <app.log>                # read-out as a key x camera table
```

`drive.sh` grants camera and microphone, launches with `--console-pty`, waits for `END` (90 s at most) or for the console
to close (the app is gone), terminates, and copies the engine log `dylib-<pid>.log` from the boot log dir (`boot`).
`MOVIE` lines also count the video samples in the file (`AVAssetReader`, no decode) and give samples / track duration
(`samples=` `fps=`, e23b). Output goes to `$SIMCAM_HOME/results/features-<rt>-<inj>-<what>-<tag>/`: `app.log`, `host.log`, `dylib.log`. Crashes:
`tools/crashes.py <since-epoch>`.
