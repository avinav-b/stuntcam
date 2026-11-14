# fixtures/flutter — Flutter camera + mobile_scanner harness

A Flutter app (`camera` / `camera_avfoundation` plus `mobile_scanner`) that runs its tests by itself when it launches. It
came from spike m7. Bundle id `dev.spike.m7cam`.

## Install dependencies and build

Point `FLUTTER_SDK` at a local Flutter SDK checkout. The one in `${STUNTCAM_WORK:-~/stuntcam-work}/flutter` (3.47.9) works. Nothing is
installed globally: `scripts/fenv.sh` sets `PUB_CACHE` / `XDG_CONFIG_HOME` under `build/flutter/`, `LANG`/`LC_ALL`
(`en_US.UTF-8`, which CocoaPods needs) and `$F`, the flutter binary.

```sh
export FLUTTER_SDK=${STUNTCAM_WORK:-~/stuntcam-work}/flutter
source ../../scripts/fenv.sh
$F pub get
(cd ios && pod install)                 # LANG/LC_ALL already set by fenv.sh; `$F build` also runs it
$F build ios --simulator --debug        # → build/ios/iphonesimulator/Runner.app
```

## How the spikes launched it

There are no launch arguments. `scripts/run.sh flutter[-<test>] <rt> boot|launch <tag>` writes the test name (default
`all`) to `Documents/m7test.txt` in the app's data container. It then runs `simctl launch` and follows
`Documents/m7.log` until `END`.

`lib/main.dart` is spike m12's version: the `scanner` test asks
`mobile_scanner` for QR and EAN-13 and passes (`scanner.qr+ean13`) once it has seen both, which the engine's default
scene (scene v2) draws. With `M12_NO_SCENE2=1` there is no EAN-13 and that step fails.
