# canary — end-to-end AVFoundation check app

A throwaway Swift app (no Xcode project) from spikes m8/m10 that drives the whole capture flow through AVFoundation's own
consumers and exits with a verdict line `RESULT core=PASS|FAIL|INCONCLUSIVE ext=PASS|FAIL|INCONCLUSIVE …` (exit 0 = PASS,
1 = FAIL, 4 = INCONCLUSIVE, 3 = its own watchdog with no FAIL recorded, also INCONCLUSIVE; a crash leaves no `RESULT` line).
INCONCLUSIVE means only timing checks failed (late frames, photos, movies; the frame gap during a photo) or the budget
(`CANARY_BUDGET`, default 45 s) ran out; something that never arrives is a FAIL. `scripts/run.sh` retries an INCONCLUSIVE
once (`SIMCAM_NO_RETRY=1` to keep the first). See the header of `main.swift` for the stages.

A photo stage (photo-jpeg, photo-4x3, photo-hvc1) whose only miss is the frame-gap check (`longest frame gap during the
photo`) is run once more at the end of the same launch, in a session of its own, with the same threshold (w4 e37, narrowed
in e38). Nothing else is retried in-process: the cold start time (frames A's first frame), frame counts and fps, a late or
undelivered photo, and any stage with another miss stay INCONCLUSIVE (the CLI relaunches the whole canary once). The
RESULT line then carries `retried:[<first attempt's misses> -> retry ok|SLOW|FAIL]` right after `t=…s`: `ok` passed on
the retry (a *retried pass*: the CLI says so and caches it for 1 h only), `SLOW` missed again (the run stays
INCONCLUSIVE, with the retry's misses in `timing:[…]`), `FAIL` hit a content check.

## Build and install

```sh
./build.sh                                              # → build/m10canary.app, bundle id dev.simcam.spike.m10canary
xcrun simctl install $SIMCAM_UDID_263 build/m10canary.app   # build.sh does not install it
```

## Run

```sh
../build.sh                                    # engine; writes build/current_dylib.txt
../scripts/run.sh canary <rt> launch <tag>     # per-launch injection, profile engine/profiles/cfg-m10.plist
../scripts/run.sh canary <rt> boot <tag>       # sim booted with the stub by scripts/boot.sh
```

The UDID comes from `$SIMCAM_UDID_<rt>` (e.g. `SIMCAM_UDID_263`). `DYLIB=<path>` and `PLIST=<path>` override the dylib and
the profile. When the canary logs `BG-REQUEST`, `run.sh` brings Preferences to the front and then relaunches the canary.
Results go to `results/canary-<rt>-<inj>-<tag>/` (`app.log`, `dylib.log`, `summary.txt` with the `RESULT` line,
`start.txt`). Crash reports aren't copied; `python3 -I ../tools/crashes.py $(cat …/start.txt)` lists them.

The older m8 runner (`canary/run.sh`, bundle id `dev.simcam.spike.m8canary`, profile `cfg-m5b.plist`) was removed;
`scripts/run.sh canary` replaces it.
