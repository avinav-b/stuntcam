---
name: stuntcam
description: Use when an iOS app's camera must work in the iOS Simulator (preview, photos, video, QR or barcode scanning), or the Simulator shows no camera or a black preview.
---

# stuntcam

Gives the iOS Simulator a camera. App code is unchanged; never mock the camera.
Supports AVFoundation, VisionCamera 5 (not 4), expo-camera, Flutter `camera` and
`mobile_scanner`. Needs Apple silicon, Xcode 27, iOS 17.5–27.2 runtime, Node.js.

## Start

1. Boot the simulator, install the app.
2. `npx stuntcam up --udid <UDID> --grant <bundle id>`
   - First run on a runtime: 30–90 s self-test. Not hung.
   - `--grant`: camera + mic allowed, no alert. Quits the app if running.
   - Launchd mode: apps launched after this get the camera (`simctl launch`,
     `expo run:ios`, `react-native run-ios`, `flutter run`). Xcode ⌘R needs
     `--reboot`; `xcodebuild test` needs `exec` (below).
3. Launch the app.
4. `npx stuntcam status --json`: `Mode` must be `boot` or `launchd`.

UDIDs: `xcrun simctl list devices booted`.

## Picture (live, no relaunch)

`npx stuntcam source <src>`, `<src>` one of:
- `scene`: default; has a QR code and an EAN-13 barcode
- `image:/path/photo.png`, `video:/path/clip.mov`
- `qr:'<text>'`: scanners report exactly `<text>`
- `webcam`: Mac camera; needs a person to allow access

Add `--udid` if several simulators are booted.

## Verify

- `npx stuntcam trace <bundle id> [--json]`: last 64 camera ops
- `xcrun simctl io <UDID> screenshot shot.png`

## UI tests

```sh
npx stuntcam up --udid "$SIM" --grant <bundle id>
npx stuntcam exec -- xcodebuild test -scheme <scheme> -destination "platform=iOS Simulator,id=$SIM"
```

Use `id=`, not `name=`. `exec` returns the command's exit code.

## Tools that inject libraries (Detox)

`npx stuntcam up --mode launchd --udid <UDID>`, then run the tool after
`eval "$(npx stuntcam env --udid <UDID>)"`. Shut down a boot-mode simulator first.

## No camera

`npx stuntcam doctor --json`; its `checks` name the problem.
- After ⌘R: `up --reboot`
- After a simulator restart: `up` again
- Tests didn't use it: `id=` destination
- App latched (2 crashes): `sentinel ls`, then `sentinel clear --bundle <bundle id>`

## Limits

4K60 records at 16–32 fps. Zoom doesn't change the picture. No 720p, Live Photo,
depth, focus lock or flash. Expo Go untested.

Off: `npx stuntcam down` (no reboot). All flags: https://github.com/avinav-b/stuntcam/blob/main/cli/README.md
