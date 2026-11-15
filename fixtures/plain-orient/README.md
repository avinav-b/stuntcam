# fixtures/plain-orient

A minimal Swift app (no Xcode project) for orientation, lifecycle and stress runs: preview, photo and movie, with capture
angles from `AVCaptureDevice.RotationCoordinator`. Result lines start with `P12|`.

```sh
SIMCAM_UDID_263=<udid> ./build.sh [tsan|asan]                          # → build/p12.app (or p12tsan / p12asan)
scripts/p12.sh <rt> orient|lifecycle|stress back|front <orient> <tag>  # from the repo root
scripts/orient-plain.sh <rt> <tag>                                     # every camera and orientation, classified
```
