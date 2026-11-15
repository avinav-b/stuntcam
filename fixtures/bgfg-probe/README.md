# fixtures/bgfg-probe

A measurement app: one capture session taken through background and foreground. It logs session notifications, scene
callbacks, its own calls and frame counts. Modes (`-mode`): `stopInBackground`, `keepRunning`, `stopWhileInterrupted`,
`appNotif`.

```sh
SIMCAM_UDID_263=<udid> fixtures/bgfg-probe/build.sh
fixtures/bgfg-probe/drive.sh <rt> <mode> <tag> boot
fixtures/bgfg-probe/matrix.sh <rt> <tag>                  # every mode
```
