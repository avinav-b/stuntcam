#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# cycle.sh <rt> vccycle|expocycle <cycles> <tag> — one launch of the RN fixture's open/close cycle mode on a boot-armed sim
#   (scripts/boot.sh <rt> stub first; for a sanitizer run, copy build/{tsan,asan}_loader.txt over
#   build/current_loader.txt before booting, or scripts/swap.sh <udid> <that loader>).
#   Metro: fixtures/expo on :$EXPO_PORT (default 8099). UDID: $SIMCAM_UDID_<rt>.
# Writes results/<mode>-<rt>-<tag>/: app.log (m1b.log), console.txt (the app's stdout/stderr, where sanitizer reports go),
#   dylib.log (this pid's dylib-<pid>.log from the boot log dir), summary.txt:
#   cycles=<ok>/<n> sessions=<adopted>/<finalized> tsan=<WARNING: ThreadSanitizer count> asan=<ERROR: AddressSanitizer count> end=<0|1>
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh; RT=$1; MODE=$2; N=$3; TAG=$4
need_udid "$0" "$RT" UDID long
B=dev.spike.m1bcam; LOGN=m1b.log; LIB=${MODE%cycle}; MAXW=${MAXW:-$(( 60 + N * 15 ))}
OUT=$SIMCAM_RESULTS/$MODE-$RT-$TAG; rm -rf $OUT; mkdir -p $OUT
xcrun simctl terminate $UDID $B 2>/dev/null
xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
DATA=$(xcrun simctl get_app_container $UDID $B data); mkdir -p $DATA/Documents; rm -f $DATA/Documents/$LOGN
typeset -a ENVS; ENVS=(); for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done
T0=$(date +%s)
env $ENVS xcrun simctl launch --console-pty $UDID $B -CAMTEST $MODE -CYCLES $N -RCT_jsLocation localhost:${EXPO_PORT:-8099} > $OUT/console.txt 2>&1 &
LP=$!; GONE=0
# not wait_for_line: 1 s polls of the app's log file, and "APP GONE" once the app is no longer running
for i in {1..$MAXW}; do sleep 1
  [[ -f $DATA/Documents/$LOGN ]] && cp $DATA/Documents/$LOGN $OUT/app.log
  grep -q "END cycle-$LIB" $OUT/app.log 2>/dev/null && break
  if (( i > 8 )) && ! xcrun simctl spawn $UDID launchctl list 2>/dev/null | grep -q "UIKitApplication:$B"; then GONE=1
    echo "APP GONE after ${i}s" >> $OUT/app.log; break; fi
done
sleep 1; xcrun simctl io $UDID screenshot $OUT/final.png >/dev/null 2>&1
xcrun simctl terminate $UDID $B 2>/dev/null; sleep 1; kill $LP 2>/dev/null
[[ -f $DATA/Documents/$LOGN ]] && cp $DATA/Documents/$LOGN $OUT/app.log
PID=$(grep -oE "^$B: [0-9]+" $OUT/console.txt | head -1 | awk '{print $2}')
BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null); [[ -n $PID && -f $BL/dylib-$PID.log ]] && cp $BL/dylib-$PID.log $OUT/dylib.log
OK=$(grep -oE 'INFO cycle ok=[0-9]+' $OUT/app.log 2>/dev/null | grep -oE '[0-9]+$'); OK=${OK:-0}
AD=$(grep -c 'adopting session' $OUT/dylib.log 2>/dev/null); FI=$(grep -c 'finalize -> forgotten' $OUT/dylib.log 2>/dev/null)
TS=$(grep -c 'WARNING: ThreadSanitizer' $OUT/console.txt 2>/dev/null); AS=$(grep -c 'ERROR: AddressSanitizer' $OUT/console.txt 2>/dev/null)
EN=$(grep -c "END cycle-$LIB" $OUT/app.log 2>/dev/null); FL=$(grep -c FAIL $OUT/app.log 2>/dev/null)
echo "cycles=$OK/$N sessions=${AD:-0}/${FI:-0} tsan=${TS:-0} asan=${AS:-0} end=${EN:-0} fail=${FL:-0} gone=$GONE pid=${PID:--} $(( $(date +%s) - T0 ))s" > $OUT/summary.txt
echo "$T0" > $OUT/start.txt
echo "$MODE/$RT/$TAG :: $(cat $OUT/summary.txt)"
