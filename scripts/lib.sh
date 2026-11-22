#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# lib.sh <rt> <test> <cam> <orient> <tag> [K=V ...] — one launch of the RN (vcorient|expoorient) or Flutter
# (flutter-<test>) fixture with per-launch injection.
#   RN: fixtures/expo from Metro on :$EXPO_PORT (default 8101; scripts/metro.sh defaults to 8099).
#   NOINJ=1: no per-launch injection (the sim was booted with the stub by boot.sh). The boot log dir's newest
#   dylib-<pid>.log is copied to dylib.log.
#   HOOK=<file in scripts/> is sourced every second, in this shell: it sees ROOT, UDID, BUNDLE, ENGINE_DYLIB and OUT.
set -u
HERE=${0:A:h}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=$1
TEST=$2
CAM=$3
ORIENTATION=$4
TAG=$5
shift 5
need_udid "$0" "$RUNTIME" UDID long

OUT=$SIMCAM_RESULTS/lib-$TEST-$RUNTIME-$CAM-$ORIENTATION-$TAG
rm -rf $OUT
mkdir -p $OUT
ENGINE_DYLIB=${DYLIB:-$(cat $ROOT/build/current_dylib.txt)}

typeset -a ARGS ENVS
ARGS=()
ENVS=()

# m7 (Flutter) and m1b (React Native): the milestones that added each app; bundle ids and log names are data and stay
if [[ $TEST == flutter* ]]; then
  BUNDLE=dev.spike.m7cam
  APP_LOG_NAME=m7.log
  END_PATTERN="END"
  FLUTTER_TEST=${TEST#flutter-}
else
  BUNDLE=dev.spike.m1bcam
  APP_LOG_NAME=m1b.log
  END_PATTERN="END $TEST"
  ARGS=(-CAMTEST $TEST -CAM $CAM -RCT_jsLocation localhost:${EXPO_PORT:-8101})
fi

# not running / already granted: nothing to report
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null

DATA=$(xcrun simctl get_app_container $UDID $BUNDLE data)
mkdir -p $DATA/Documents
rm -f $DATA/Documents/$APP_LOG_NAME
[[ $TEST == flutter* ]] && echo $FLUTTER_TEST > $DATA/Documents/m7test.txt

ENVS=(SIMCTL_CHILD_SIMCAM_CONTROL=$OUT/control.txt)
if [[ ${NOINJ:-0} == 0 ]]; then
  inject_env "$ENGINE_DYLIB" "$SIMCAM_DEV/profiles/cfg-m10.plist" "$OUT/dylib.log"
  ENVS+=($INJECT_ENV)
fi
[[ $ORIENTATION != - ]] && ENVS+=(SIMCTL_CHILD_SIMCAM_ORIENT=$ORIENTATION)
for kv in "$@"; do   # -Arg value words go to the app's arguments, K=V to its environment
  if [[ $kv == -* ]]; then
    ARGS+=(${(s: :)kv})   # "-Arg value", split at the spaces
  else
    ENVS+=(SIMCTL_CHILD_$kv)
  fi
done

: > $OUT/control.txt
env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE $ARGS > $OUT/launch.txt 2>&1 &
LAUNCH_PID=$!
START_S=$(date +%s)
typeset -A shot

# Not wait_for_line: this polls every second (MAXW times, default 120). Each poll copies the app's log file, takes
# the screenshots the app asks for (once each) and runs the HOOK.
for i in {1..${MAXW:-120}}; do
  sleep 1
  [[ -f $DATA/Documents/$APP_LOG_NAME ]] && cp $DATA/Documents/$APP_LOG_NAME $OUT/app.log
  # app.log may not exist yet: no screenshots asked for, and no end line, this poll
  for s in $(grep -oE 'SCREENSHOT [a-z0-9-]+' $OUT/app.log 2>/dev/null | awk '{print $2}'); do
    if [[ -z ${shot[$s]:-} ]]; then
      # a screenshot is evidence only: one that fails changes no verdict
      xcrun simctl io $UDID screenshot $OUT/$s.png >/dev/null 2>&1
      shot[$s]=1
    fi
  done
  [[ -n ${HOOK:-} ]] && source "$HERE/$HOOK"
  grep -q "$END_PATTERN" $OUT/app.log 2>/dev/null && break
done

sleep 1
# a screenshot is evidence only: one that fails changes no verdict
xcrun simctl io $UDID screenshot $OUT/final.png >/dev/null 2>&1
# whatever already ended: nothing to report
kill $LAUNCH_PID 2>/dev/null
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null

for f in $(grep -oE 'path=[^ ]+' $OUT/app.log 2>/dev/null | cut -d= -f2 | sed 's#^file://##'); do
  [[ -f $f ]] && cp $f $OUT/
done

if [[ ${NOINJ:-0} != 0 ]]; then   # Copies the newest engine log in the boot log dir (if there is none, nothing).
  # no file or no logs: nothing to copy
  BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)
  NEWEST_LOG=$(ls -t $BOOT_LOG_DIR/dylib-*.log 2>/dev/null | head -1)
  [[ -n $NEWEST_LOG ]] && cp $NEWEST_LOG $OUT/dylib.log
fi

PASSES=$(grep -c PASS $OUT/app.log)
FAILS=$(grep -c FAIL $OUT/app.log)
echo "$TEST/$RUNTIME/$CAM/$ORIENTATION/$TAG :: pass=$PASSES fail=$FAILS $(( $(date +%s) - START_S ))s"
