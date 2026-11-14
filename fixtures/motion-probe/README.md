# motion-probe (w4 e24, design §11 U2)

A measurement app, not a test. Two `CMMotionManager` instances (A and B) each record accelerometer updates with
`startAccelerometerUpdates(to:)` on their own serial `OperationQueue`, at `-hz` (100) for `-dur` s (10). With `-camera 1`
an `AVCaptureSession` (back wide camera, `.hd1920x1080`, 30 fps, `AVCaptureVideoDataOutput`) runs at the same time.
`-order motionFirst|cameraFirst` picks which starts first; the other follows 0.5 s later. The `Camera` object, and so its
`AVCaptureSession` (one of the engine's lazy-arming entry points), is only created at the camera step. Results:
measured internally, W4 e24.

Every line is `PROBE| <s since launch> <text>`:
- `STEP motion|camera`: when each step starts.
- `AVAIL@first …`: availability (accel, gyro, mag, deviceMotion) on manager A: the app's first CoreMotion query.
- `A|B start interval=… accelActive=…`, `ACTIVE@start`: the accelerometer `…Active` flags right after starting.
- `MGR A|B count=…`: per manager, over the window [start, start + dur]: updates delivered (`count`; `total` includes the
  few that arrive while stopping), `rate` (Hz) and `pct` of the requested rate, mean and min interval between data
  timestamps, `maxGap` (longest wait between handler calls, including from start to the first and from the last to the
  window's end), `firstLatency`, `tsOff*` (handler-time `ProcessInfo.systemUptime` − `CMLogItem.timestamp`: first, last,
  max |·|), `tsFirst` / `upAtFirst` (the raw values, so the clock is visible), `nonMonotonic` timestamps, handler errors,
  the first gravity vector, and whether `accelerometerData` (pull) was non-nil at the end.
- `CAM …`: session running, format, frames in the window and fps.
- `AVAIL@end …`: availability on a fresh manager C after the window (after arming, if the camera ran).
- `ACTIVE@attempt …`: C's `…Active` flags 0.3 s after `startGyroUpdates(to:)`, `startMagnetometerUpdates(to:)` and
  `startDeviceMotionUpdates(to:)`; `DELIVERED@attempt 2s …`: what each of those delivered in 2 s.
- `END`, then the app exits.

```sh
SIMCAM_UDID_263=… SIMCAM_UDID_27=… SIMCAM_UDID_175=… fixtures/motion-probe/build.sh   # build/motionprobe.app (dev.simcam.e24.motionprobe), installs on booted sims
XENV=SIMCAM_LAZY=0 fixtures/motion-probe/drive.sh <rt> <tag> boot -camera 1 -order motionFirst   # one launch; SIMCAM_LAZY=0 = eager, SIMCAM=0 = unarmed
fixtures/motion-probe/matrix.sh <rt> <tag> [N=20] [UNARMED=5]       # lazy|eager x camera 0|1 x order, N each, interleaved; then unarmed
python3 -I fixtures/motion-probe/summarize.py "$SIMCAM_HOME/results" <tag>   # one markdown row per runtime and variant
```

`drive.sh` grants camera and motion with `xcrun simctl privacy` before each launch and copies the launch's engine log
(`dylib-<pid>.log` from the boot log dir) when there is one; a lazy launch that never touches the camera writes none.
