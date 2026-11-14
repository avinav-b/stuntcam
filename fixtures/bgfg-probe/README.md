# bgfg-probe (w3 e20)

A measurement app, not a test: one `AVCaptureSession` (back wide camera input + `AVCaptureVideoDataOutput`) taken through
background and foreground. It logs and checks nothing. Results: measured internally, W3 e20.

Every line is `PROBE| <CFAbsoluteTime> <s since launch> <text>`. CFAbsoluteTime is the clock of the engine log
(`M5_LOG`, `engine/src/simcam.m` `LOG`), so `merge.py` can merge the two by time. The probe logs:
- UIScene callbacks (`SCENE …`), plus the four `UIApplication` notifications (`APP …`) the engine itself observes;
- every `AVCaptureSession` notification (`NOTE …`: DidStartRunning, DidStopRunning, WasInterrupted with its reason,
  InterruptionEnded, RuntimeError with its error), with `isRunning`/`isInterrupted` read in the handler, and KVO changes
  of both (`KVO …`);
- its own calls (`CALL` / `RET`, with the state before and after and how long the call took). Calls run on a serial
  session queue, as Apple recommends;
- frames per 250 ms window (`frames N /250ms`). A window is logged when its count differs from the previous one, and every
  1 s while frames arrive.

Behaviour (launch argument `-mode`):

| mode | background | foreground |
|---|---|---|
| `stopInBackground` | `stopRunning` in `sceneDidEnterBackground` | `startRunning` in `sceneWillEnterForeground`; fg+3 s stop, fg+5 s start, fg+7 s stop ("the later stop") |
| `keepRunning` | nothing | nothing; fg+4 s one `stopRunning`, to see whether a stop still reaches the engine |
| `stopWhileInterrupted` | `stopRunning` 1 s after `WasInterrupted` (if no WasInterrupted arrives: 1.5 s after `sceneDidEnterBackground`) | fg+3 s start, fg+5 s stop |
| `appNotif` (extra) | `stopRunning` from a `UIApplication.didEnterBackgroundNotification` observer | `startRunning` from a `willEnterForeground` observer; then as `stopInBackground` |

Timing: 3 s after the first `didBecomeActive` the probe prints `BG-REQUEST`, and the host backgrounds it. The probe holds a
`UIApplication` background task from `didEnterBackground` to `willEnterForeground`, so it keeps running and logging in the
background. `END` marks the end of its script.

```sh
SIMCAM_UDID_27=… SIMCAM_UDID_263=… fixtures/bgfg-probe/build.sh        # builds build/bgfgprobe.app (dev.simcam.e20.bgfgprobe), installs on booted sims
fixtures/bgfg-probe/drive.sh <rt> <mode> <tag> boot|none                # one run; XENV=M12_NO_INTERRUPT=1, BGS=<s in background>
fixtures/bgfg-probe/matrix.sh <rt> <tag>                                # e20's runs on one runtime (boots the sim itself)
fixtures/bgfg-probe/matrix-extra.sh <rt> <tag>                          # the appNotif runs
python3 -I fixtures/bgfg-probe/condense.py "$SIMCAM_HOME/results" > c.txt && python3 -I fixtures/bgfg-probe/calls.py c.txt
```

`drive.sh` backgrounds the probe with `xcrun simctl launch <udid> com.apple.Preferences`. After `BGS` s (6 in e20) it runs
`xcrun simctl launch <udid> dev.simcam.e20.bgfgprobe`, which brings the running probe back to the foreground without
relaunching it. After `END` it measures the probe's CPU time over 3 s and takes a 2 s `/usr/bin/sample`. Stacks in
`M5Tick`/`__M5StartPump_block_invoke` show the engine is still pumping frames for a session that AVFoundation reports as
stopped. Output goes to `$SIMCAM_HOME/results/bgfg-<rt>-<inj>-<mode>-<tag>/`: `app.log`, `host.log`, `dylib.log`,
`sample.txt` and `timeline.txt` (from `merge.py`).
