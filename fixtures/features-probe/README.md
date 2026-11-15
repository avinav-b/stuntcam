# fixtures/features-probe

A measurement app: it reads which camera features AVFoundation reports as supported, and records what happens when an
app turns each one on (Live Photo, depth, RAW, flash, bracketing, MultiCam, HDR and more). It logs; it doesn't pass or
fail. `scripts/features.sh` uses it to check that unsupported features stay unsupported.

```sh
SIMCAM_UDID_263=<udid> fixtures/features-probe/build.sh
fixtures/features-probe/drive.sh <rt> boot <tag> read                       # what the camera reports
fixtures/features-probe/drive.sh <rt> boot <tag> feature <name> [camera]    # turn one feature on
fixtures/features-probe/matrix.sh <rt> <tag>                                # every feature
python3 -I fixtures/features-probe/summarize.py <app.log>
```

Feature names are defined in `main.swift`.
