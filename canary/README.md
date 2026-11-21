# canary

The self-test app `stuntcam up` runs before it arms a simulator. A small Swift app (no Xcode project) that drives the
whole capture flow through AVFoundation and prints one line: `RESULT core=PASS|FAIL|INCONCLUSIVE ext=…`.

Exit codes: 0 pass, 1 fail, 4 inconclusive (only timing checks missed, or the `CANARY_BUDGET` of 45 s ran out), 3 its own
watchdog. The checks are listed at the top of `main.swift`; `Canary.run()` is the order they run in, one function
per stage.

```sh
./build.sh                                    # → build/m10canary.app
xcrun simctl install <udid> build/m10canary.app
../scripts/run.sh canary <rt> boot <tag>      # on a simulator booted by scripts/boot.sh
```

`<rt>` picks the simulator from `$SIMCAM_UDID_<rt>` (for example `SIMCAM_UDID_263`). Results go to
`results/canary-<rt>-boot-<tag>/`.
