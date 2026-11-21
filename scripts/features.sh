#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# features.sh <rt> — do the camera features stuntcam can't deliver read as unsupported? (design §6.5; L53, L54)
#   On a sim armed by scripts/boot.sh <rt> stub, runs the features probe (fixtures/features-probe) twice through its
#   drive.sh: one read-out (`read`) and one AVCam-style "enable if supported" launch (`feature ifsupported`, back-wide).
#   FAIL if, on any camera the read-out lists:
#     bracket   photo maxBracketedCapturePhotoCount is not 0, before or after startRunning           [L53]
#     multicam  supportedMultiCamDeviceSets is not []                                               [L54]
#   or if the ifsupported launch turned on anything but flash (its RESULT enabled= is not "none" or "flash"), hung, threw,
#   or took fewer than 5/5 photos. Also FAIL if the read-out has no END, lists no camera, or the torch is gone from the back cameras
#   (hasTorch must stay YES on every back camera: the torch works, the canary checks it). Flash is offered on the back
#   cameras and never fires (a documented limit, design §6.5; L55): reported, and hasFlash must stay YES on the back
#   cameras so apps that set flashMode .on without checking don't throw. The MultiCam class flag
#   (AVCaptureMultiCamSession.isMultiCamSupported) is the simulator's own and is only reported (design §6.5).
#   Prints one PASS/FAIL line per check and exits 1 if any failed (2 on a usage error).
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to both launches (e.g. XENV=SIMCAM_OFF_BRACKET_UNSUPPORTED=1 must FAIL bracket).
#   UDID: $SIMCAM_UDID_<rt>. Needs fixtures/features-probe/build/featuresprobe.app (fixtures/features-probe/build.sh);
#   installed here. Results: $SIMCAM_RESULTS/features-<rt>-boot-{read,ifsupported-back-wide}-<tag>/.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh
RT=${1:?usage: $0 <rt>}
need_udid "$0" "$RT" UDID long
BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null); [[ -n $BL && -d $BL ]] ||
  { echo "$0: no boot log dir: boot with scripts/boot.sh $RT stub first" >&2; exit 2; }
APP=$ROOT/fixtures/features-probe/build/featuresprobe.app
[[ -d $APP ]] || { echo "$0: $APP missing: run fixtures/features-probe/build.sh" >&2; exit 2; }
xcrun simctl install $UDID $APP || { echo "$0: installing the probe failed" >&2; exit 2; }
TAG=feat-$(date +%H%M%S); D=$ROOT/fixtures/features-probe
$D/drive.sh $RT boot $TAG read >/dev/null
$D/drive.sh $RT boot $TAG feature ifsupported back-wide >/dev/null
python3 -I - "$SIMCAM_RESULTS/features-$RT-boot-read-$TAG/app.log" "$SIMCAM_RESULTS/features-$RT-boot-ifsupported-back-wide-$TAG/app.log" <<'PY'
import re, sys
def lines(p):
    try: return [m.group(1) for l in open(p, errors='replace') for m in [re.match(r'PROBE\| \S+ +\S+ (.*)$', l.rstrip('\r\n'))] if m]
    except OSError: return []
rd, fs = lines(sys.argv[1]), lines(sys.argv[2])
R = {}   # (camera, object, key) -> value, configuration-1 read-out
RUN = {}  # values logged again after startRunning (object photo-run)
for t in rd:
    p = t.split(' ', 4)
    if p[0] == 'R' and len(p) == 5:
        R.setdefault((p[1], p[2], p[3]), p[4])
        if p[2] == 'photo-run': RUN.setdefault((p[1], p[3]), p[4])
cams = next((t.split()[2:] for t in rd if t.startswith('CAMERAS ')), [])
fails = 0
def check(name, ok, info):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {info}"); fails += not ok
if not any(t.startswith('END') for t in rd) or not cams:
    check('read-out', False, f"no END or no camera ({len(rd)} lines, cameras {cams})"); sys.exit(1)
bad = {c: R.get((c, 'photo', 'maxBracketedCapturePhotoCount')) for c in cams}
run = {c: v for (c, k), v in RUN.items() if k == 'maxBracketedCapturePhotoCount' and v != '0'}   # set back on start?
check('bracket', all(v == '0' for v in bad.values()) and not run, 'maxBracketedCapturePhotoCount ' + ', '.join(f'{c}={v}' for c, v in bad.items())
      + (' | after startRunning: ' + ', '.join(f'{c}={v}' for c, v in run.items()) if run else ''))
sets = R.get(('-', 'multicam', 'supportedMultiCamDeviceSets'))
check('multicam', sets == '[]', f"supportedMultiCamDeviceSets {sets if sets is None or len(sets) < 120 else sets[:120] + '…'} "
      f"(class isMultiCamSupported {R.get(('-', 'multicam', 'AVCaptureMultiCamSession.isMultiCamSupported'))}, the simulator's own)")
fl = {c: (R.get((c, 'device', 'hasFlash')), R.get((c, 'photo', 'supportedFlashModes'))) for c in cams}
backfl = {c: h for c, (h, m) in fl.items() if c.startswith('back-')}
check('flash kept', bool(backfl) and all(h == 'YES' for h in backfl.values()), 'hasFlash/supportedFlashModes ' + ', '.join(f'{c}={h}/{m}' for c, (h, m) in fl.items()) + ' (offered, never fires: a documented limit)')
tor = {c: R.get((c, 'device', 'hasTorch')) for c in cams if c.startswith('back-')}
check('torch kept', bool(tor) and all(v == 'YES' for v in tor.values()), 'hasTorch ' + ', '.join(f'{c}={v}' for c, v in tor.items()))
res = next((t for t in fs if t.startswith('RESULT ')), None)
m = res and re.search(r'enabled=(\S+) captures=(\d+)/5 hangs=(\d+) exceptions=(\d+)', res)
check('ifsupported', bool(m) and m.group(1) in ('none', 'flash') and m.group(2) == '5' and m.group(3) == '0' and m.group(4) == '0',
      res[len('RESULT '):] if res else 'no RESULT (app gone or hung)')
sys.exit(1 if fails else 0)
PY
