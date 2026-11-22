#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# cycle.sh <rt> vccycle|expocycle <cycles> <tag> — one launch of the RN fixture's open/close cycle mode
#   on a boot-armed sim (scripts/boot.sh <rt> stub first; for a sanitizer run, copy build/{tsan,asan}_loader.txt over
#   build/current_loader.txt before booting, or scripts/swap.sh <udid> <that loader>).
#   Metro: fixtures/expo on :$EXPO_PORT (default 8099). UDID: $SIMCAM_UDID_<rt>.
# Writes results/<mode>-<rt>-<tag>/: app.log (m1b.log), console.txt (the app's stdout/stderr, where sanitizer
#   reports go), dylib.log (this pid's dylib-<pid>.log from the boot log dir), final.png, start.txt, summary.txt:
#   cycles=<ok>/<n> sessions=<adopted>/<finalized> tsan=<WARNING: ThreadSanitizer count>
#     asan=<ERROR: AddressSanitizer count> end=<0|1> fail=<FAIL lines> gone=<0|1> pid=<pid> <elapsed>s
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=$1
MODE=$2
CYCLES=$3
TAG=$4
need_udid "$0" "$RUNTIME" UDID long

# the React Native app (fixtures/expo); m1b is a historical name, kept because the app uses the id and log name
BUNDLE=dev.spike.m1bcam
APP_LOG_NAME=m1b.log
LIB=${MODE%cycle}
# MAXW (an input) is how many 1 s polls wait for the app's END line: by default a minute plus 15 s per cycle.
WAIT_BASE_S=60
WAIT_PER_CYCLE_S=15
MAXW=${MAXW:-$(( WAIT_BASE_S + CYCLES * WAIT_PER_CYCLE_S ))}
GONE_GRACE_S=8   # the app may not be in launchctl's list yet during its first seconds

OUT=$SIMCAM_RESULTS/$MODE-$RUNTIME-$TAG
rm -rf $OUT
mkdir -p $OUT

# not running / already granted: nothing to report
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null

DATA=$(xcrun simctl get_app_container $UDID $BUNDLE data)
mkdir -p $DATA/Documents
rm -f $DATA/Documents/$APP_LOG_NAME

typeset -a ENVS
ENVS=()
for kv in ${=XENV:-}; do
  ENVS+=(SIMCTL_CHILD_$kv)
done

START_S=$(date +%s)
env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE -CAMTEST $MODE -CYCLES $CYCLES \
  -RCT_jsLocation localhost:${EXPO_PORT:-8099} > $OUT/console.txt 2>&1 &
LAUNCH_PID=$!

GONE=0
# Not wait_for_line: this polls every second, copies the app's log file, and logs "APP GONE" once the app is no
# longer running.
for i in {1..$MAXW}; do
  sleep 1
  [[ -f $DATA/Documents/$APP_LOG_NAME ]] && cp $DATA/Documents/$APP_LOG_NAME $OUT/app.log
  # app.log may not exist yet, and the sim may not answer: try again next poll
  grep -q "END cycle-$LIB" $OUT/app.log 2>/dev/null && break
  if (( i > GONE_GRACE_S )) &&
    ! xcrun simctl spawn $UDID launchctl list 2>/dev/null | grep -q "UIKitApplication:$BUNDLE"; then
    GONE=1
    echo "APP GONE after ${i}s" >> $OUT/app.log
    break
  fi
done

sleep 1
# a screenshot is evidence only: one that fails changes no verdict; the app may have ended
xcrun simctl io $UDID screenshot $OUT/final.png >/dev/null 2>&1
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
sleep 1
# whatever already ended: nothing to report
kill $LAUNCH_PID 2>/dev/null

[[ -f $DATA/Documents/$APP_LOG_NAME ]] && cp $DATA/Documents/$APP_LOG_NAME $OUT/app.log
PID=$(grep -oE "^$BUNDLE: [0-9]+" $OUT/console.txt | head -1 | awk '{print $2}')
# no file: no engine log to copy; app.log may be missing too, which leaves OK at 0
BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)
[[ -n $PID && -f $BOOT_LOG_DIR/dylib-$PID.log ]] && cp $BOOT_LOG_DIR/dylib-$PID.log $OUT/dylib.log

OK=$(grep -oE 'INFO cycle ok=[0-9]+' $OUT/app.log 2>/dev/null | grep -oE '[0-9]+$')
OK=${OK:-0}
# The counts below are empty if the file is missing (grep's error is expected then).
ADOPTED=$(grep -c 'adopting session' $OUT/dylib.log 2>/dev/null)
FINALIZED=$(grep -c 'finalize -> forgotten' $OUT/dylib.log 2>/dev/null)
TSAN_WARNINGS=$(grep -c 'WARNING: ThreadSanitizer' $OUT/console.txt 2>/dev/null)
ASAN_ERRORS=$(grep -c 'ERROR: AddressSanitizer' $OUT/console.txt 2>/dev/null)
ENDS=$(grep -c "END cycle-$LIB" $OUT/app.log 2>/dev/null)
FAILS=$(grep -c FAIL $OUT/app.log 2>/dev/null)

SUMMARY="cycles=$OK/$CYCLES sessions=${ADOPTED:-0}/${FINALIZED:-0} tsan=${TSAN_WARNINGS:-0} asan=${ASAN_ERRORS:-0}"
SUMMARY+=" end=${ENDS:-0} fail=${FAILS:-0} gone=$GONE pid=${PID:--} $(( $(date +%s) - START_S ))s"
echo "$SUMMARY" > $OUT/summary.txt
echo "$START_S" > $OUT/start.txt
echo "$MODE/$RUNTIME/$TAG :: $(cat $OUT/summary.txt)"
