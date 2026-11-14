# fixtures/plain — plain AVFoundation app

A minimal Swift app (no Xcode project) that runs AVFoundation checks against the cameras the engine provides: device
discovery, session start, photo capture (it **waits** for `didFinishCaptureFor`), movie recording, metadata. It came from
spike m9 and was extended in m10.

## Build and install

```sh
export SIMCAM_UDID_263=<udid> SIMCAM_UDID_27=<udid> SIMCAM_UDID_175=<udid>   # any subset
./build.sh        # swiftc → build/m5app.app (bundle id dev.simcam.spike.m10plain), ad-hoc signed, installed on those sims
```

No dependencies besides Xcode's iOS Simulator SDK.

## How the spikes launched it

`scripts/run.sh plain <rt> boot|launch <tag>` runs `xcrun simctl launch --console-pty <udid> dev.simcam.spike.m10plain`,
waits for `DONE` and counts the `| PASS` / `| FAIL` lines. There are no launch arguments. The env var `M5APP_MODE=scan|q4`
picks the mode (pass it with `XENV="M5APP_MODE=…"` to `run.sh`).

## Photo then stop (w4 e40, ledger L66)

`M5APP_PHOTOSTOP=bg|main|incallback` replaces the photo and movie steps: one photo, then `stopRunning` while it is in
flight, from a background thread (`bg`), on the main thread (`main`; AVFoundation's photo delegate queue is the main
queue), or inside `didFinishProcessingPhoto` (`incallback`). `photostop.*` lines: the photo and `didFinishCapture` arrive,
`stopRunning` returns in < 900 ms (no hang, and no wait for callbacks that can only run after it returns), the session is
stopped (and `isRunning` is false as `stopRunning` returns), and with `bg` `didFinishCapture` arrived before `stopRunning`
returned.

w4 e44 (review) adds two modes:
- `mainsync`: main runs `sessionQueue.sync { stopRunning() }`, the main thread under another queue. Same checks; before
  e44 the engine waited its full 1 s there (`photostop.stop` FAIL).
- `restart`: the stop on a background thread and, 30 ms later, `startRunning` from another thread while the stop still
  waits for the photo's callbacks (run it with `M40_FINISH_DELAY_MS=300` to hold the stop open). AVFoundation doesn't
  forward that start (17.5, 26.3, 27.0: `isRunning` is still true while a stop waits, and the start returns without
  starting), so `photostop.restart.*` checks that the session stays coherent (`isRunning` agrees with frames, no
  DidStartRunning followed by DidStopRunning), and that a start after the stop resumes frames, with DidStartRunning last.

## Open/close cycles in one process (w4 e58, ledger L75)

`M5APP_MODE=cycle` opens and closes the camera `M5APP_CYCLES` times (default 50) in one process: each cycle builds a new
session (back wide input, video data output at its default format, photo output, preview layer), waits for 10 frames,
stops it and drops everything. `M5APP_CYCLE_PHOTO=1` adds a photo per cycle, `M5APP_CYCLE_MOVIE=1` a 1 s movie,
`M5APP_CYCLE_IDLE=<s>` the idle after each cycle (default 0.4), `M5APP_CYCLE_WAIT=<s>` an idle before the first (time to
attach Instruments), `M5APP_CYCLE_HOLD=1` keeps the process at the end. After each cycle the app prints
`MEM cycle=<i> fp_kb=<phys_footprint>` (its own `task_info` reading) and an Instruments Points of Interest event `cycle`;
at the end `INFO cycle ok=<ok>/<n> fp0_kb=… fp_end_kb=… slope_kb=<KB per cycle>`. `scripts/memcycle.sh` runs it and
asserts that the footprint stays flat.

## Ordering modes (design §10.3, U2/U13)

`M5APP_ORDER=<mode>` (e.g. `XENV="M5APP_ORDER=motion" scripts/run.sh plain 263 boot ord-motion`) does one thing before
the normal checks, and adds `order.*` PASS/FAIL lines:

| mode | before the first camera call |
|---|---|
| `motion` | one `CMMotionManager`, push accelerometer updates at 100 Hz: available, ≥90% of the rate, systemUptime timestamps |
| `motion2` | a two-manager motion recorder: a pull manager plus a push manager on an `OperationQueue`, both running through the camera checks |
| `audio` | `AVCaptureDevice.default(for: .audio)` must return a device |
| `session` | an `AVCaptureSession` exists before discovery (without the mode, discovery runs first) |
| `threads` | three threads make their first capture call at the same moment |

Gyro, magnetometer and device-motion availability are printed as `INFO` lines, not gated yet (design §6.1 says they
should report unavailable).

## The 14 checks (m10; one `PASS|FAIL <name>` line each)

`discovery` (devices found), `default.back` (default back wide camera), `formats` (count, max photo dims), `lock+zoom`
(zoom 2 reads back), `torch`, `session.config` (input + VDO + photo + movie outputs added), `session.start` (running, ms),
`frames` (>20 VDO frames in 2 s, first frame dims), `preview.contents` (preview layer has contents), `focus.kvo`
(isAdjustingFocus changed ≥2 times after a focus point), `photo` (JPEG bytes and dims), `photo.didFinishCapture` (waits
on `captureOutput:didFinishCaptureFor:`, which hung before m10), `movie` (2 s recording, duration > 1 s), `session.stop`.
`lock` and `input` only appear on failure paths.
