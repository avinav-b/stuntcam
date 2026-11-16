<!-- SPDX-License-Identifier: Apache-2.0 -->
# stuntcam

A camera for the iOS Simulator. Your app's camera code runs unmodified, through Apple's own AVFoundation.

```sh
npx stuntcam up
```

Then run your app as usual. `npx stuntcam down` turns it off; `npx stuntcam doctor` says why if there's no camera.

Your Mac's camera: `npx stuntcam source webcam`.

Needs an Apple silicon Mac and Xcode 27. Simulator only: nothing is linked into your app. Run it with `npx` rather than
adding it as a dependency: the package is macOS-only, so `npm ci` on Linux would fail.

Docs: https://github.com/avinav-b/stuntcam · Apache-2.0
