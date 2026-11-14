#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# lib.sh <rt> <test> <cam> <orient> <tag> [K=V ...] — RN (vcorient|expoorient) or Flutter (flutter-<test>) under per-launch injection
#   RN: fixtures/expo from Metro on :$EXPO_PORT (default 8101, m12's port; scripts/metro.sh defaults to 8099)
#   NOINJ=1: no per-launch injection (sim booted with the stub by boot.sh); the boot log dir's dylib-<pid>.log is copied to dylib.log
set -u; HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; T=$2; CAM=$3; OR=$4; TAG=$5; shift 5
V=SIMCAM_UDID_$RT; UDID=${(P)V:-}; [[ -n $UDID ]] || { echo "$0: set $V to the UDID of your iOS $RT simulator" >&2; exit 2; }
OUT=$SIMCAM_RESULTS/lib-$T-$RT-$CAM-$OR-$TAG; rm -rf $OUT; mkdir -p $OUT
DY=${DYLIB:-$(cat $ROOT/build/current_dylib.txt)}
typeset -a ARGS ENVS; ARGS=(); ENVS=()
if [[ $T == flutter* ]]; then B=dev.spike.m7cam; LOGN=m7.log; ENDRE="END"; FT=${T#flutter-}
else B=dev.spike.m1bcam; LOGN=m1b.log; ENDRE="END $T"; ARGS=(-CAMTEST $T -CAM $CAM -RCT_jsLocation localhost:${EXPO_PORT:-8101}); fi
xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
DATA=$(xcrun simctl get_app_container $UDID $B data); mkdir -p $DATA/Documents; rm -f $DATA/Documents/$LOGN
[[ $T == flutter* ]] && echo $FT > $DATA/Documents/m7test.txt
ENVS=(SIMCTL_CHILD_SIMCAM_CONTROL=$OUT/control.txt)
[[ ${NOINJ:-0} == 0 ]] && ENVS+=(SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=$DY SIMCTL_CHILD_M2_FAKE=1 SIMCTL_CHILD_M2_SESSION_HOOK=1 SIMCTL_CHILD_M2_NOLOGDICT=1
  SIMCTL_CHILD_M2_PLIST=$SIMCAM_DEV/profiles/cfg-m10.plist SIMCTL_CHILD_M5_LOG=$OUT/dylib.log)
[[ $OR != - ]] && ENVS+=(SIMCTL_CHILD_SIMCAM_ORIENT=$OR)
for kv in "$@"; do [[ $kv == -* ]] && ARGS+=(${(s: :)kv}) || ENVS+=(SIMCTL_CHILD_$kv); done
: > $OUT/control.txt
env $ENVS xcrun simctl launch --console-pty $UDID $B $ARGS > $OUT/launch.txt 2>&1 &
LP=$!; T0=$(date +%s); typeset -A shot
for i in {1..${MAXW:-120}}; do sleep 1
  [[ -f $DATA/Documents/$LOGN ]] && cp $DATA/Documents/$LOGN $OUT/app.log
  for s in $(grep -oE 'SCREENSHOT [a-z0-9-]+' $OUT/app.log 2>/dev/null | awk '{print $2}'); do
    [[ -z ${shot[$s]:-} ]] && { xcrun simctl io $UDID screenshot $OUT/$s.png >/dev/null 2>&1; shot[$s]=1; }; done
  [[ -n ${HOOK:-} ]] && source $HERE/$HOOK
  grep -q "$ENDRE" $OUT/app.log 2>/dev/null && break
done
sleep 1; xcrun simctl io $UDID screenshot $OUT/final.png >/dev/null 2>&1; kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null
for f in $(grep -oE 'path=[^ ]+' $OUT/app.log 2>/dev/null | cut -d= -f2 | sed 's#^file://##'); do [[ -f $f ]] && cp $f $OUT/; done
if [[ ${NOINJ:-0} != 0 ]]; then BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null); L=$(ls -t $BL/dylib-*.log 2>/dev/null | head -1); [[ -n $L ]] && cp $L $OUT/dylib.log; fi
echo "$T/$RT/$CAM/$OR/$TAG :: pass=$(grep -c PASS $OUT/app.log) fail=$(grep -c FAIL $OUT/app.log) $(( $(date +%s) - T0 ))s"
