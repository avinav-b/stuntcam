#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# features.sh <rt> — do the camera features stuntcam can't deliver read as unsupported?
#   On a sim armed by scripts/boot.sh <rt> stub, runs the features probe (fixtures/features-probe) twice through its
#   drive.sh: one read-out (`read`) and one AVCam-style "enable if supported" launch (`feature ifsupported`, back-wide).
#   FAIL if, on any camera the read-out lists:
#     bracket   photo maxBracketedCapturePhotoCount is not 0, before or after startRunning
#     multicam  supportedMultiCamDeviceSets is not []
#   or if the ifsupported launch turned on anything but flash (its RESULT enabled= is not "none" or "flash"), hung,
#   threw, or took fewer than 5/5 photos. Also FAIL if the read-out has no END, lists no camera, or the torch is gone
#   from the back cameras (hasTorch must stay YES on every back camera: the torch works, the canary checks it). Flash
#   is offered on the back cameras and never fires (a documented limit): reported, and hasFlash must
#   stay YES on the back cameras so apps that set flashMode .on without checking don't throw. The MultiCam class flag
#   (AVCaptureMultiCamSession.isMultiCamSupported) is the simulator's own and is only reported.
#   Prints one PASS/FAIL line per check and exits 1 if any failed (2 on a usage error).
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to both launches
#   (e.g. XENV=SIMCAM_OFF_BRACKET_UNSUPPORTED=1 must FAIL bracket).
#   UDID: $SIMCAM_UDID_<rt>. Needs fixtures/features-probe/build/featuresprobe.app (fixtures/features-probe/build.sh);
#   installed here. Results: $SIMCAM_RESULTS/features-<rt>-boot-{read,ifsupported-back-wide}-<tag>/.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=${1:?usage: $0 <rt>}
need_udid "$0" "$RUNTIME" UDID long

BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)   # no file: checked next
[[ -n $BOOT_LOG_DIR && -d $BOOT_LOG_DIR ]] ||
  { echo "$0: no boot log dir: boot with scripts/boot.sh $RUNTIME stub first" >&2; exit 2; }

APP=$ROOT/fixtures/features-probe/build/featuresprobe.app
[[ -d $APP ]] || { echo "$0: $APP missing: run fixtures/features-probe/build.sh" >&2; exit 2; }
xcrun simctl install $UDID $APP || { echo "$0: installing the probe failed" >&2; exit 2; }

TAG=feat-$(date +%H%M%S)
PROBE_DIR=$ROOT/fixtures/features-probe
$PROBE_DIR/drive.sh $RUNTIME boot $TAG read >/dev/null
$PROBE_DIR/drive.sh $RUNTIME boot $TAG feature ifsupported back-wide >/dev/null

READ_LOG=$SIMCAM_RESULTS/features-$RUNTIME-boot-read-$TAG/app.log
IFSUPPORTED_LOG=$SIMCAM_RESULTS/features-$RUNTIME-boot-ifsupported-back-wide-$TAG/app.log

python3 -I - "$READ_LOG" "$IFSUPPORTED_LOG" <<'PY'
import re
import sys


def lines(path):
    """The text of each 'PROBE| <time> <tag> <text>' line of a probe log ([] if it can't be read)."""
    try:
        return [match.group(1) for line in open(path, errors='replace')
                for match in [re.match(r'PROBE\| \S+ +\S+ (.*)$', line.rstrip('\r\n'))] if match]
    except OSError:
        return []


read_lines, ifsupported_lines = lines(sys.argv[1]), lines(sys.argv[2])
readout = {}   # (camera, object, key) -> value, from the configuration-1 read-out
after_start = {}   # (camera, key) -> value logged again after startRunning (object photo-run)
for text in read_lines:
    # "R <camera> <object> <key> <value>"
    fields = text.split(' ', 4)
    if fields[0] == 'R' and len(fields) == 5:
        _, camera, obj, key, value = fields
        readout.setdefault((camera, obj, key), value)
        if obj == 'photo-run':
            after_start.setdefault((camera, key), value)
cameras = next((text.split()[2:] for text in read_lines if text.startswith('CAMERAS ')), [])
fails = 0


def check(name, ok, info):
    global fails
    print(f"{'PASS' if ok else 'FAIL'} {name}: {info}")
    fails += not ok


def pairs(values):
    """'camera=value, ...' for a dict."""
    return ', '.join(f'{camera}={value}' for camera, value in values.items())


if not any(text.startswith('END') for text in read_lines) or not cameras:
    check('read-out', False, f"no END or no camera ({len(read_lines)} lines, cameras {cameras})")
    sys.exit(1)

bracket_counts = {camera: readout.get((camera, 'photo', 'maxBracketedCapturePhotoCount')) for camera in cameras}
# Was it set back on start?
set_back = {camera: value for (camera, key), value in after_start.items()
            if key == 'maxBracketedCapturePhotoCount' and value != '0'}
check('bracket', all(value == '0' for value in bracket_counts.values()) and not set_back,
      'maxBracketedCapturePhotoCount ' + pairs(bracket_counts)
      + (' | after startRunning: ' + pairs(set_back) if set_back else ''))

MAX_SHOWN = 120   # characters of the device-set list that are printed
device_sets = readout.get(('-', 'multicam', 'supportedMultiCamDeviceSets'))
class_flag = readout.get(('-', 'multicam', 'AVCaptureMultiCamSession.isMultiCamSupported'))
shown = device_sets if device_sets is None or len(device_sets) < MAX_SHOWN else device_sets[:MAX_SHOWN] + '…'
check('multicam', device_sets == '[]',
      f"supportedMultiCamDeviceSets {shown} (class isMultiCamSupported {class_flag}, the simulator's own)")

flash = {camera: (readout.get((camera, 'device', 'hasFlash')), readout.get((camera, 'photo', 'supportedFlashModes')))
         for camera in cameras}
back_has_flash = {camera: has for camera, (has, modes) in flash.items() if camera.startswith('back-')}
check('flash kept', bool(back_has_flash) and all(has == 'YES' for has in back_has_flash.values()),
      'hasFlash/supportedFlashModes ' + ', '.join(f'{camera}={has}/{modes}' for camera, (has, modes) in flash.items())
      + ' (offered, never fires: a documented limit)')

back_has_torch = {camera: readout.get((camera, 'device', 'hasTorch')) for camera in cameras
                  if camera.startswith('back-')}
check('torch kept', bool(back_has_torch) and all(value == 'YES' for value in back_has_torch.values()),
      'hasTorch ' + pairs(back_has_torch))

result = next((text for text in ifsupported_lines if text.startswith('RESULT ')), None)
match = result and re.search(r'enabled=(\S+) captures=(\d+)/5 hangs=(\d+) exceptions=(\d+)', result)
check('ifsupported',
      bool(match) and match.group(1) in ('none', 'flash') and match.group(2) == '5'
      and match.group(3) == '0' and match.group(4) == '0',
      result[len('RESULT '):] if result else 'no RESULT (app gone or hung)')
sys.exit(1 if fails else 0)
PY
