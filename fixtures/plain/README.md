# fixtures/plain

A minimal Swift app (no Xcode project) that checks the camera through plain AVFoundation: discovery, session, photo,
movie and metadata. Each check prints `PASS|FAIL <name>`.

```sh
SIMCAM_UDID_263=<udid> ./build.sh          # → build/m5app.app, installed on the simulators you name
scripts/run.sh plain <rt> boot <tag>       # from the repo root
```

Other modes, set with `XENV="…"` on `run.sh`:

| Variable | What it does |
|---|---|
| `M5APP_MODE=scan` | barcode scanning |
| `M5APP_MODE=cycle` | opens and closes the camera 50 times and prints memory per cycle (`scripts/memcycle.sh`) |
| `M5APP_MODE=lidar` | the LiDAR camera: formats, photos, depth output |
| `M5APP_MODE=webcam` | the live webcam source: fps, duplicates, gaps and latency from the helper's frame stamp and from each frame's presentation time, offline-band frames, a photo and a 5 s movie (`webcam.swift`; `WC_*` settings at its top) |
| `M5APP_PHOTOSTOP=bg\|main\|incallback\|mainsync\|restart` | stops the session while a photo is in flight |
| `M5APP_ORDER=motion\|motion2\|audio\|session\|threads` | does one thing before the first camera call |
