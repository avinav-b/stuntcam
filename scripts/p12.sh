#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# p12.sh <rt> <mode> <cam> <orient> <tag> [K=V ...]  — per-launch injection of the current dylib into the plain12 app
#   orient: portrait|landscapeLeft|landscapeRight (virtual device orientation via SIMCAM_ORIENT) ; extra K=V go to the app env
#   app: fixtures/plain-orient (build.sh installs it); UDID: $SIMCAM_UDID_<rt> (e.g. SIMCAM_UDID_263); HOOK=<file in scripts/> is sourced every 0.2 s
#   NOINJ=1: no per-launch injection (sim booted with the stub by boot.sh); the boot log dir's dylib-<pid>.log is copied to dylib.log
set -u; HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh
source $ROOT/scripts/common.sh; RT=$1; MODE=$2; CAM=$3; OR=$4; TAG=$5; shift 5
need_udid "$0" "$RT" UDID long; B=${P12_BID:-dev.simcam.spike.m12plain}
OUT=$SIMCAM_RESULTS/p12-$MODE-$RT-$CAM-$OR-$TAG; rm -rf $OUT; mkdir -p $OUT
DY=${DYLIB:-$(cat $ROOT/build/current_dylib.txt)}
xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl privacy $UDID grant camera $B 2>/dev/null
xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
DATA=$(xcrun simctl get_app_container $UDID $B data); rm -f $DATA/Documents/*(N)
typeset -a ENVS; ENVS=(SIMCTL_CHILD_P12_MODE=$MODE SIMCTL_CHILD_P12_CAM=$CAM SIMCTL_CHILD_SIMCAM_CONTROL=$OUT/control.txt)
[[ ${NOINJ:-0} == 0 ]] && { inject_env "$DY" "$SIMCAM_DEV/profiles/cfg-m10.plist" "$OUT/dylib.log"; ENVS+=($INJECT_ENV); }
[[ $OR != - ]] && ENVS+=(SIMCTL_CHILD_SIMCAM_ORIENT=$OR)
for kv in "$@"; do ENVS+=(SIMCTL_CHILD_$kv); done
: > $OUT/control.txt
env $ENVS xcrun simctl launch --console-pty $UDID $B > $OUT/app.log 2>&1 &
LP=$!; T0=$(date +%s)
# not wait_for_line: 0.2 s polls (MAXW of them, default 600: 120 s), the preview screenshot and the HOOK each time
for i in {1..${MAXW:-600}}; do sleep 0.2
  if grep -q "SCREENSHOT preview" $OUT/app.log && [[ ! -f $OUT/preview.png ]]
  then xcrun simctl io $UDID screenshot $OUT/preview.png >/dev/null 2>&1; touch $DATA/Documents/shot.ack; fi
  [[ -n ${HOOK:-} ]] && source $HERE/$HOOK
  grep -q "P12| DONE" $OUT/app.log && break; kill -0 $LP 2>/dev/null || break
done
sleep 0.5; kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null
for f in photo.jpg movie.mov; do [[ -f $DATA/Documents/$f ]] && cp $DATA/Documents/$f $OUT/; done
if [[ ${NOINJ:-0} != 0 ]]; then BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)
  L=$(ls -t $BL/dylib-*.log 2>/dev/null | head -1); [[ -n $L ]] && cp $L $OUT/dylib.log; fi
echo "$MODE/$RT/$CAM/$OR/$TAG :: $(grep -cE 'P12\| (FAIL)' $OUT/app.log) fails, $(( $(date +%s) - T0 ))s"
