#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# test.sh <rt> <patched|control> <tag> [shots] — install an AVCam build (build.sh) on our sim and tap its shutter with the
#   XCUITest in uitest/ (xcodebuild test-without-building; no GUI clicks). The sim must be booted by scripts/boot.sh <rt> stub.
#   UDID: $SIMCAM_UDID_<rt>. Variant: patched = build/patched (both patch parts), control = build/control (--no-livephoto-fix).
#   Fresh install each run (so AVCam's Live Photo toggle is at its default, on); camera, microphone, photos-add and location
#   granted with simctl privacy before launch. Shots: default 20.
#   Writes $SIMCAM_RESULTS/avcam-<rt>-<variant>-<tag>/: xcodebuild.log, shutter.txt (the test's SHUTTER| lines), photos.txt
#   (the sim's DCIM before/after), crashes.txt (tools/crashes.py over the run), dylib-*.log (engine logs of AVCam), tree.txt,
#   before.png / after.png. Last line: one summary line. Exit 0 only if the run did what the variant should: patched =
#   xcodebuild passed, `shots` new photos, no AVCam crash; control = AVCam crashed (the Live Photo exception) and xcodebuild
#   failed. Exit 1 otherwise.
set -u; zmodload zsh/datetime
D=${0:A:h}; ROOT=${D:h:h}; source $ROOT/scripts/env.sh; RT=$1; VAR=$2; TAG=$3; SHOTS=${4:-20}
V=SIMCAM_UDID_$RT; UDID=${(P)V:-}; [[ -n $UDID ]] || { echo "$0: set $V" >&2; exit 2; }
APP=$D/build/$VAR/dd/Build/Products/Debug-iphonesimulator/AVCam.app; B=com.example.apple-samplecode.AVCam
XTR=$(ls $D/build/uitest-dd/Build/Products/*.xctestrun 2>/dev/null | head -1)
[[ -d $APP ]] || { echo "$0: no $APP (run build.sh)" >&2; exit 2; }
[[ -n $XTR ]] || { echo "$0: no .xctestrun (build uitest/ first, see README)" >&2; exit 2; }
OUT=$SIMCAM_RESULTS/avcam-$RT-$VAR-$TAG; rm -rf $OUT; mkdir -p $OUT
DCIM=$HOME/Library/Developer/CoreSimulator/Devices/$UDID/data/Media/DCIM
count() { find $DCIM -type f \( -iname '*.heic' -o -iname '*.jpg' -o -iname '*.mov' \) 2>/dev/null | wc -l | tr -d ' '; }
SINCE=$EPOCHSECONDS; touch $OUT/.since
xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl uninstall $UDID $B 2>/dev/null
xcrun simctl install $UDID $APP || { echo "$0: install failed" >&2; exit 1; }
for s in camera microphone photos-add location; do xcrun simctl privacy $UDID grant $s $B || echo "privacy grant $s failed" >> $OUT/host.log; done
N0=$(count)
# TEST_RUNNER_<VAR> in xcodebuild's environment reaches the test as <VAR>
# -collect-test-diagnostics never: after an app crash, xcodebuild otherwise waits 600 s for a sysdiagnose of the simulator
mkdir -p $D/build/xcresult; rm -rf $D/build/xcresult/$RT-$VAR-$TAG.xcresult
TEST_RUNNER_SHOTS=$SHOTS TEST_RUNNER_OUTDIR=$OUT xcodebuild test-without-building -xctestrun $XTR -destination id=$UDID \
  -collect-test-diagnostics never -derivedDataPath $D/build/uitest-dd -resultBundlePath $D/build/xcresult/$RT-$VAR-$TAG.xcresult \
  > $OUT/xcodebuild.log 2>&1; RC=$?
sleep 3   # let Photos finish writing the last asset and ReportCrash write any report
N1=$(count)
grep -a 'SHUTTER|' $OUT/xcodebuild.log | sed 's/.*SHUTTER| //' | awk '!seen[$0]++' > $OUT/shutter.txt
{ echo "before $N0 after $N1 new $((N1 - N0))"; find $DCIM -type f -newer $OUT/.since 2>/dev/null | sed "s#$DCIM/##" | sort; } > $OUT/photos.txt
python3 -I $ROOT/tools/crashes.py $SINCE > $OUT/crashes.txt 2>&1
BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)
for f in $BL/dylib-*.log(N); do [[ $(stat -f %m $f) -ge $SINCE ]] && grep -qa 'AVCam' $f && cp $f $OUT/; done
xcrun simctl terminate $UDID $B 2>/dev/null
CR=$(grep -c "'$B'" $OUT/crashes.txt)
NEW=$((N1 - N0))
echo "$RT/$VAR/$TAG xcodebuild=$RC taps=$(grep -oE 'done taps=[0-9]+/[0-9]+' $OUT/shutter.txt | cut -d= -f2) new-photos=$((N1 - N0)) avcam-crashes=$CR toggle=$(grep -oE "livephoto-toggle label='[^']*'" $OUT/shutter.txt | head -1 | cut -d"'" -f2) :: $(grep -aE 'Test (Case|Suite).*(passed|failed)' $OUT/xcodebuild.log | tail -1)"
if [[ $VAR == patched ]]; then (( RC == 0 && NEW == SHOTS && CR == 0 )) && exit 0
else (( RC != 0 && CR > 0 )) && exit 0; fi
echo "$0: FAIL ($VAR: xcodebuild=$RC new-photos=$NEW/$SHOTS avcam-crashes=$CR)" >&2; exit 1
