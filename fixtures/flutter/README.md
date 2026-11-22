# fixtures/flutter

A Flutter app (`camera` and `mobile_scanner`) that runs its camera tests when it launches.

```sh
export FLUTTER_SDK=<path to a Flutter SDK>
source ../../scripts/fenv.sh          # local caches only; sets $F to the flutter binary
$F pub get
$F build ios --simulator --debug      # → build/ios/iphonesimulator/Runner.app
```

`scripts/run.sh flutter[-<test>] <rt> boot <tag>` (from the repo root) runs one test (default `all`) and follows the
app's log. `flutter-qr` reports the QR payloads mobile_scanner sees, for `scripts/qr-test.sh`.
