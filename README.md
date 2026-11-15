# stuntcam

A camera for the iOS Simulator. Your app's camera code runs unmodified, through Apple's own AVFoundation.

```sh
npx stuntcam up
```

Then run your app as usual. `npx stuntcam down` turns it off.

Needs an Apple silicon Mac and Xcode 27. Works with AVFoundation, VisionCamera 5, expo-camera 58 and Flutter
`camera` / `mobile_scanner`, on iOS 17.5 to 27.2. No code changes.

## Usage

| | |
|---|---|
| Xcode | Run `stuntcam up` before ⌘R |
| React Native / Expo | `stuntcam up`, then `npx expo run:ios` |
| Flutter | `stuntcam up`, then `flutter run` |
| UI tests | `stuntcam exec -- xcodebuild test …` |
| Your own picture | `stuntcam source image:/path/photo.png` (or `video:`) |
| No camera? | `stuntcam doctor` |

All commands: [cli/README.md](cli/README.md). Problems: [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

## Limits

- No live webcam yet (planned for 1.1)
- 4K60 records at about 16–32 fps
- No 720p format; zoom isn't shown; flash never fires

## License

Apache-2.0
