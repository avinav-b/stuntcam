# fixtures/plain-orient — plain AVFoundation orientation / lifecycle / stress app

A minimal Swift app (no Xcode project) from spike m12 (`plain12/`): preview layer + photo + movie, with capture angles
from `AVCaptureDevice.RotationCoordinator`. It runs by itself when launched; every result line starts with `P12|`.

## Build and install

```sh
export SIMCAM_UDID_263=<udid> SIMCAM_UDID_27=<udid>   # any subset
./build.sh [tsan|asan]   # swiftc → build/p12[tsan|asan].app (bundle id dev.simcam.spike.m12plain[tsan|asan]), installed on those sims
```

The sanitizer variants copy the sanitizer runtime into the app's `Frameworks/`.

## How the spikes launched it

`scripts/p12.sh <rt> <mode> <cam> <orient> <tag> [K=V ...]` launches it with per-launch injection and writes
`results/p12-<mode>-<rt>-<cam>-<orient>-<tag>/`. Env (set by `p12.sh` or passed as `K=V`):

- `P12_MODE=orient|lifecycle|stress`, `P12_CAM=back|front`
- `orient`: `P12_HOLD` (s), `P12_MIRROR=1` (force mirrored photo/movie connections)
- `lifecycle`: `P12_ROUNDS` (default 3), `P12_AUDIO=1`; use with `HOOK=lc-hook.sh`
- `stress`: `P12_N` cycles (default 200)
- `P12_BID` (in `p12.sh`) picks the bundle id, e.g. `dev.simcam.spike.m12plaintsan`

`scripts/orient-plain.sh <rt> <tag>` runs back/front × portrait/landscapeLeft/landscapeRight and classifies the preview
screenshot, photo and movie with `tools/bin/upright` (run `tools/build.sh` first) + `tools/orient.py`. `HOOK=src-hook.sh` switches sources live through the
control file (needs `fixtures/media/still.png`, which is not committed).
