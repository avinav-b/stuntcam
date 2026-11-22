# fixtures/expo

An Expo app (SDK 58, React Native 0.88) with VisionCamera 5.2.4 and expo-camera 58 that tests the camera when it
launches. Results are printed with an `M1B|` prefix.

```sh
npm ci
(cd ios && LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 pod install)
npx expo run:ios
```

Start Metro with `scripts/metro.sh start`, then `scripts/run.sh <mode> <rt> boot <tag>` from the repo root:

| Mode | Tests |
|---|---|
| `vc`, `expo` | VisionCamera and expo-camera basics (`vc-audio` and `expo-audio` record sound) |
| `vcfull` | 102 checks of a typical VisionCamera screen; known failures are listed in `VCF_EXPECTED` in `App.tsx` |
| `vcorient`, `expoorient` | orientation of preview, photo and movie |
| `vccycle`, `expocycle` | opens and closes the camera 20 times |
| `vclidar` | the LiDAR camera (launch argument `-CAMTEST vclidar`) |
| `vcqr`, `expoqr` | the QR payloads VisionCamera's object output or expo-camera's scanner reports, for `scripts/qr-test.sh` (launch arguments `-QRPOS back\|front`, `-QRSECS N`) |
