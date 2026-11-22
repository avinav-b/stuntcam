# canary

The self-test app `stuntcam up` runs before it arms a simulator. A small Swift app (no Xcode project) that drives the
whole capture flow through AVFoundation and prints one line: `RESULT core=PASS|FAIL|INCONCLUSIVE ext=…`.

Exit codes: 0 pass, 1 fail, 4 inconclusive (only timing checks missed, or the `CANARY_BUDGET` of 45 s ran out), 3 its own
watchdog. The checks are listed at the top of `Canary.swift`; `Canary.run()` is the order they run in, one function per
stage.

| file | what it holds |
|---|---|
| `main.swift` | the app entry point: start time, scene delegate, the budget watchdog |
| `Config.swift` | the environment settings (`CANARY_BUDGET`, `CANARY_QR`, `CANARY_FIRST_FRAME_S`, `SIMCAM_QR_TEXT`) |
| `Canary.swift` | the `Canary` class: the state the AVFoundation callbacks fill, and `run()` |
| `Report.swift` | the verdict rules, the check lines, the `Verdict` (stage name and failure lists) and the `RESULT` lines |
| `Support.swift` | small helpers: host clock, polling waits, device configuration lock, format queries |
| `Delegates.swift` | the AVFoundation delegate callbacks (frames, metadata, photo, movie, audio) |
| `Frames.swift` | frame records and the first-frame, frame-window and preview checks |
| `Discovery.swift` | stage 1: device discovery and the virtual devices of the 8-camera profile |
| `MainSession.swift` | stages 2–4, 6 and 8 on the first session: configure, start, frames, QR, reconfigure, background |
| `Photo.swift` | stage 5 and 7b: one photo capture and its checks, the three photo stages, capture-then-stop |
| `Retry.swift` | the end-of-run retry of a photo stage whose only miss was the frame gap |
| `Trace.swift` | the engine's operation trace for each photo (ext) |
| `DeviceControls.swift` | focus, exposure, torch, zoom and exposure bias (ext) |
| `Movie.swift` | stage 7: a short recording and its track |
| `Teardown.swift` | stage 9: teardown and three more sessions in a row |
| `FrontMirror.swift` | the front camera and its mirroring |
| `Audio.swift` | microphone input and audio buffers (ext) |

```sh
./build.sh                                    # → build/m10canary.app
xcrun simctl install <udid> build/m10canary.app
../scripts/run.sh canary <rt> boot <tag>      # on a simulator booted by scripts/boot.sh
```

`<rt>` picks the simulator from `$SIMCAM_UDID_<rt>` (for example `SIMCAM_UDID_263`). Results go to
`results/canary-<rt>-boot-<tag>/`.
