# fixtures/avcam

Builds Apple's [AVCam sample](https://developer.apple.com/documentation/avfoundation/avcam-building-a-camera-app) with
a one-line fix so it runs under stuntcam. This directory holds a patch and scripts, not Apple's code.

stuntcam reports Live Photo as unsupported. AVCam's Live Photo button still asks for a Live Photo movie, AVFoundation
throws, and the app crashes at the first shutter. The patch asks for the movie only when Live Photo is enabled. Turning
AVCam's Live Photo button off works too. A second part of the patch lets the sample build with Xcode 27.0.

```sh
fixtures/avcam/build.sh                   # downloads the sample, patches and builds it
fixtures/avcam/build.sh <zip or folder>   # or uses your copy
fixtures/avcam/test.sh <rt> patched <tag> # 20 shutter presses in a UI test (build uitest/ first, see test.sh)
```

The app lands in `fixtures/avcam/build/patched/dd/Build/Products/Debug-iphonesimulator/AVCam.app`.

Licence: the sample is Apple's (`LICENSE.txt` in its zip). In `avcam.patch` the context and removed lines are Apple's;
the added lines and the scripts are Apache-2.0.
