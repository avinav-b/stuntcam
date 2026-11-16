# helper: Stuntcam Camera

A small macOS app (`LSUIElement`, no UI) that publishes one Mac camera into the shared-memory ring the engine's
`webcam` source reads. The ring contract is [docs/webcam-ring.md](../docs/webcam-ring.md); the C side of it
(`engine/src/webcam/ring.c`, `ring_shm.c`) is compiled into both the helper and the engine.

```sh
helper/build.sh                     # → build/helper/Stuntcam Camera.app, copied to $SIMCAM_HOME/helper/<sha>/
helper/dev.sh start --synthetic     # a moving test pattern, no camera and no permission prompt
helper/dev.sh start [--camera ID]   # the Mac camera (asks for camera permission the first time)
helper/dev.sh status | stop
helper/build.sh --release           # unsigned dev.stuntcam.camera bundle + entitlements.plist, for the release signer
```

Apps then select it with `SIMCAM_SOURCE=webcam` or the control file's `source=webcam` (the default camera, named in
`$SIMCAM_HOME/webcam/default`), or `source=webcam:<uniqueID>`; the synthetic helper's uniqueID is `synthetic`.

What it does:

- **Launch.** Only through LaunchServices: `open -n -g "<absolute path>/Stuntcam Camera.app" --args --home "$SIMCAM_HOME" …`.
  That makes the helper its own responsible process, so the camera prompt names "Stuntcam Camera" and the grant is
  its own. Never exec the binary: the launching process (a terminal) would be charged for the camera instead.
- **Camera list.** `--list PATH` writes the cameras (uniqueID, default flag, Continuity flag, type, name) to PATH and
  exits, without the lock, a pid file or a permission request; `stuntcam source webcam:NAME` resolves NAME with it.
- **One helper per camera.** It holds `flock` on `$SIMCAM_HOME/webcam/<cam>.lock` for its whole life (retried for 250 ms)
  and writes `<cam>.pid` (`pid`, `sha`, `rings`, `auth`, `state`, `camera`), rewritten on every change.
- **Permission** is settled at launch: `requestAccess` when not yet determined (`state=prompting`, `auth=prompting`), then
  `authorized`, or `denied` and exit.
- **Camera.** By uniqueID. The default is the built-in camera, else the first external one; a Continuity Camera or
  Desk View is used only when named. Center Stage is turned off, and `NSCameraReactionEffectGesturesEnabledDefault`
  is off in its Info.plist. BGRA at the format's own size (the largest at or under `--size`, default 1920×1080, at
  `--fps`, default 30). Geometry, epoch and sequence numbers stay in the helper's private memory.
- **Readers.** Engines write `$SIMCAM_HOME/webcam/readers/<udid>-<pid>` every second. A file counts only while its pid is
  alive. The camera opens while some reader is `active=1`, closes 10 s after the last one (slots cleared, `state=idle`),
  and the helper quits 10 min after the last reader of any kind.
- **Recovery.** Capture restarts on a session runtime error, after the camera is reconnected, after the Mac wakes, and
  when its own frames stop for 2 s while a reader is active.
- **Stop.** SIGTERM (or the idle quit) retires the ring: `state=stopped`, slots cleared, then the name unlinked; then the
  pid file is removed.

Log: `$SIMCAM_HOME/logs/webcam-<cam>.log`. Dev builds use the bundle id `dev.stuntcam.camera.dev` and are ad-hoc
signed (hardened runtime, camera entitlement only), so macOS asks again after a rebuild that changes the binary.
