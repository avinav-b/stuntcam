#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# p12.sh <rt> <mode> <cam> <orient> <tag> [K=V ...]  — per-launch injection of the current dylib into the plain12 app
#   orient: portrait|landscapeLeft|landscapeRight (the virtual device orientation, through SIMCAM_ORIENT).
#     Extra K=V go to the app's environment.
#   app: fixtures/plain-orient (build.sh installs it). UDID: $SIMCAM_UDID_<rt> (e.g. SIMCAM_UDID_263).
#   HOOK=<file in scripts/> is sourced every 0.2 s, in this shell: it sees ROOT, UDID, BUNDLE, ENGINE_DYLIB and OUT.
#   NOINJ=1: no per-launch injection (the sim was booted with the stub by boot.sh). The boot log dir's newest
#     dylib-<pid>.log is copied to dylib.log.
set -u
HERE=${0:A:h}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=$1
MODE=$2
CAM=$3
ORIENTATION=$4
TAG=$5
shift 5
need_udid "$0" "$RUNTIME" UDID long

# plain-orient's bundle id. "p12" and "plain12" (p12.app, the P12_* inputs, the "P12| " log lines) and "m12" are
# historical names, kept because apps and logs use them.
BUNDLE=${P12_BID:-dev.simcam.spike.m12plain}
OUT=$SIMCAM_RESULTS/p12-$MODE-$RUNTIME-$CAM-$ORIENTATION-$TAG
rm -rf $OUT
mkdir -p $OUT
ENGINE_DYLIB=${DYLIB:-$(cat $ROOT/build/current_dylib.txt)}

# not running / already granted: nothing to report
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null

DATA=$(xcrun simctl get_app_container $UDID $BUNDLE data)
rm -f $DATA/Documents/*(N)

typeset -a ENVS
ENVS=(SIMCTL_CHILD_P12_MODE=$MODE SIMCTL_CHILD_P12_CAM=$CAM SIMCTL_CHILD_SIMCAM_CONTROL=$OUT/control.txt)
if [[ ${NOINJ:-0} == 0 ]]; then
  inject_env "$ENGINE_DYLIB" "$SIMCAM_DEV/profiles/cfg-m10.plist" "$OUT/dylib.log"
  ENVS+=($INJECT_ENV)
fi
[[ $ORIENTATION != - ]] && ENVS+=(SIMCTL_CHILD_SIMCAM_ORIENT=$ORIENTATION)
for kv in "$@"; do
  ENVS+=(SIMCTL_CHILD_$kv)
done

: > $OUT/control.txt
env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE > $OUT/app.log 2>&1 &
LAUNCH_PID=$!
START_S=$(date +%s)

# Not wait_for_line: this polls every 0.2 s (MAXW times, default 600: 120 s), and each poll takes the preview
# screenshot when the app asks for it and runs the HOOK.
for i in {1..${MAXW:-600}}; do
  sleep 0.2
  if grep -q "SCREENSHOT preview" $OUT/app.log && [[ ! -f $OUT/preview.png ]]; then
    # a screenshot is evidence only: one that fails changes no verdict
    xcrun simctl io $UDID screenshot $OUT/preview.png >/dev/null 2>&1
    touch $DATA/Documents/shot.ack
  fi
  [[ -n ${HOOK:-} ]] && source "$HERE/$HOOK"
  grep -q "P12| DONE" $OUT/app.log && break
  kill -0 $LAUNCH_PID 2>/dev/null || break
done

sleep 0.5
# whatever already ended: nothing to report
kill $LAUNCH_PID 2>/dev/null
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null

for f in photo.jpg movie.mov; do
  [[ -f $DATA/Documents/$f ]] && cp $DATA/Documents/$f $OUT/
done

if [[ ${NOINJ:-0} != 0 ]]; then   # Copies the newest engine log in the boot log dir (if there is none, nothing).
  # no file or no logs: nothing to copy
  BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)
  NEWEST_LOG=$(ls -t $BOOT_LOG_DIR/dylib-*.log 2>/dev/null | head -1)
  [[ -n $NEWEST_LOG ]] && cp $NEWEST_LOG $OUT/dylib.log
fi

FAIL_LINES=$(grep -cE 'P12\| (FAIL)' $OUT/app.log)
echo "$MODE/$RUNTIME/$CAM/$ORIENTATION/$TAG :: $FAIL_LINES fails, $(( $(date +%s) - START_S ))s"
