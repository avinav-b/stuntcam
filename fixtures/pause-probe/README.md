# fixtures/pause-probe (w4 e25, U5)

A measurement app, not a test. It has one `AVCaptureSession`: back wide camera, VDO, photo output at max dims, movie
file output and preview. The host drives it, so it can pause the app at a known moment (lldb breakpoint, background,
`kill -STOP`) and check that every operation still completes. Results: measured internally, W4 e25.

- `main.swift`: the host appends commands (`<seq> <cmd> [arg]`) to `Documents/cmd.txt` in the app container, and the
  app polls it every 20 ms.
  - Commands: `setup [4k60]`, `photo <n>`, `armresign <n>` / `armbg <n>` (a burst inside the next
    `sceneWillResignActive` / `sceneDidEnterBackground`), `focus`, `rec start|stop`, `stop`, `start`, `status`, `exit`.
  - Every line is `PP| <CFAbsoluteTime> <s since launch> <text>`, the engine log's clock.
  - Breakpoint targets (plain C symbols, built `-Onone`): `probe_photo_hit`, `probe_kvo_hit` (isAdjustingFocus),
    `probe_kvo_running_hit` (session isRunning) and `probe_vdo_hit`.
- `hold.py`: an lldb SB-API holder, run with Xcode's python3 and `PYTHONPATH=$(xcrun lldb -P)`. It attaches by pid and
  holds the breakpoint 3 times for 10 s each, then detaches.
- `onmatch.py`: sends SIGSTOP to a pid within about 2 ms of a matching app log line.
- `lib.sh`, `drive.sh <rt> <case>`: one case. Before any attach or signal they check that the pid is this probe on our
  own simulator. Output goes to `$SIMCAM_RESULTS/<PP_RUN>-<case>-<rt>/`.
- `summarize.py`: one markdown row per run.
- `selftest.sh <rt>` (w4 e27): checks drive.sh itself. It forces a wait timeout (the case must FAIL), and sends
  SIGINT/SIGTERM mid-SIGSTOP and mid-lldb-hold (the app must be resumed, with no lldb left).

A timed-out wait, a refused stop or attach, or a failed hold.py fails the case: drive.sh prints `FAIL <case>/<rt>: …` and
exits 1. A passing case ends with `PASS <case>/<rt> :: …`. `PP_RUN` (default `e25`) sets the result-dir prefix.

```sh
SIMCAM_UDID_27=… SIMCAM_UDID_263=… fixtures/pause-probe/build.sh   # builds and installs on those (booted) sims
fixtures/pause-probe/drive.sh 27 c3-photo
```
