# stuntcam

A camera for the iOS Simulator. Your app's camera code runs unmodified, through Apple's own AVFoundation: previews,
photos, video, and QR and barcode scanning.

```sh
npx stuntcam up      # arm a simulator
                     # then run your app as usual
npx stuntcam down    # turn it off
```

Needs an Apple silicon Mac with Xcode 27. Works with AVFoundation, VisionCamera 5, expo-camera 58, and Flutter `camera`
and `mobile_scanner`, on iOS 17.5 to 27.2. No code, dependency or project changes.

- **Xcode:** run `stuntcam up` before Run (⌘R). If Xcode or Simulator rebooted the simulator, run `stuntcam up --reboot`.
- **UI tests:** `npx stuntcam exec -- xcodebuild test -scheme MyApp -destination 'platform=iOS Simulator,id=<UDID>'`
- **No camera?** `npx stuntcam doctor` says why.

## Limits

- No live webcam yet (planned for 1.1). The picture is a test scene, or your own image or video (`--source`).
- 4K60 records at about 16–32 fps. No 720p format. Zoom is accepted but not shown. Flash never fires.

## Docs

[Usage and commands](docs/USAGE.md) · [Troubleshooting](docs/TROUBLESHOOTING.md) ·
[How it works](docs/ARCHITECTURE.md) · [Support](docs/SUPPORT.md)

## License

Apache-2.0. Third-party files: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
