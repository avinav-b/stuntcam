# fixtures/pause-probe

A measurement app for pauses: the host stops it at a known moment (a debugger breakpoint, backgrounding, `kill -STOP`)
and checks that every camera operation still completes. Commands reach the app through a file in its container (see
`main.swift`).

```sh
SIMCAM_UDID_263=<udid> fixtures/pause-probe/build.sh
fixtures/pause-probe/drive.sh <rt> <case>       # prints PASS or FAIL
fixtures/pause-probe/selftest.sh <rt>           # checks drive.sh itself
```
