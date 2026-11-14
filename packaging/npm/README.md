<!-- SPDX-License-Identifier: Apache-2.0 -->
# stuntcam

A camera for the iOS Simulator. Your app's real camera code runs unmodified: no code, dependency or project changes. The
simulator shows a synthetic scene (or your image or video) through Apple's own AVFoundation, so previews, photos, video,
QR and barcode scanning and orientation behave the way they do on a phone.

Needs an Apple-silicon Mac with Xcode 27 and an iOS Simulator runtime (verified: 26.3 and 27.0; 17.5 smoke-tested).
Simulator only: nothing is linked into your app, and device builds never load it.

```sh
npx stuntcam up                    # no install; or a global install. A devDependency makes `npm ci` fail on Linux CI (the package is darwin/arm64 only)
npx stuntcam up                    # self-test, then arm a simulator (boots it with the camera if it's shut down)
# run your app as usual: xcodebuild, expo run:ios, react-native run-ios, flutter run, Xcode ⌘R
npx stuntcam status                # what each simulator is doing
npx stuntcam doctor                # why there's no camera, if there isn't
npx stuntcam down                  # stop: new launches get no camera, no reboot
```

UI tests: `npx stuntcam exec -- xcodebuild test -scheme <UI test scheme> -destination id=<udid>`.

The package is the `stuntcam` command (a native macOS arm64 binary, no install scripts) plus the prebuilt simulator
libraries and the self-test app (inside `share/stuntcam/payload.tar`, byte for byte as built and signed) and the restore
rules, under `share/stuntcam/`. At each use stuntcam checks every file against `share/stuntcam/manifest.txt`, whose
sha256 is built into the `stuntcam` binary (so the files are exactly the ones packaged with it), and unpacks what
simulators read into `~/Library/Application Support/stuntcam`. It uses its own files even when run inside a stuntcam
source checkout, unless `STUNTCAM_USE_CHECKOUT=1` or `--build` says otherwise. `stuntcam version` names the release and the commit it was built from.

Licence: Apache-2.0 (`LICENSE`); third-party notices: `THIRD_PARTY_NOTICES.md`. Docs, support policy and troubleshooting:
https://github.com/avinav-b/stuntcam
