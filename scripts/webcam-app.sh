#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# webcam-app.sh <rt> <tag> [K=V ...] — one run of fixtures/plain in M5APP_MODE=webcam on the sim $SIMCAM_UDID_<rt> (armed
# with scripts/boot.sh <rt> stub). Source: $SRC (default webcam:synthetic), set through a per-run control file, so a test
# can switch it mid-run (source=...); $ORIENT adds orientation=$ORIENT. Extra K=V go to the app (WC_SECS, WC_MOVIE, WC_PHOTO, WC_W/WC_H, ...).
# Writes $SIMCAM_RESULTS/webcam-<rt>-<tag>/: app.log, dylib.log, control.txt, cpu.txt (app CPU % over the measured
# window: from wc.running + 1 s to the VDO WCSTAT line), and prints the WCSTAT lines and the engine's webcam lines.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh; RT=$1; TAG=$2; shift 2
need_udid "$0" "$RT" U short
OUT=$SIMCAM_RESULTS/webcam-$RT-$TAG; rm -rf $OUT; mkdir -p $OUT
CTL=$OUT/control.txt; print -r -- "source=${SRC:-webcam:synthetic}" > $CTL; [[ -n ${ORIENT:-} ]] && print -r -- "orientation=$ORIENT" >> $CTL
B=dev.simcam.spike.m10plain; typeset -a ENVS; ENVS=(SIMCTL_CHILD_M5APP_MODE=webcam "SIMCTL_CHILD_SIMCAM_CONTROL=$CTL")
for kv in "$@"; do ENVS+=("SIMCTL_CHILD_$kv"); done
xcrun simctl terminate $U $B 2>/dev/null; xcrun simctl privacy $U grant camera $B 2>/dev/null; xcrun simctl privacy $U grant microphone $B 2>/dev/null
env $ENVS xcrun simctl launch --console-pty $U $B > $OUT/app.log 2>&1 &
LP=$!
# up to 10 s for the engine's first line (its pid), then up to 15 s for the app's wc.running, and 1 s more
PID=; for i in {1..100}; do sleep 0.1; PID=$(grep -m1 -oE '\[simcam [0-9]+\]' $OUT/app.log | grep -oE '[0-9]+'); [[ -n $PID ]] && break; done
for i in {1..150}; do sleep 0.1; grep -q "wc.running" $OUT/app.log && break; done; sleep 1
C0=$(ps -o time= -p ${PID:-0} 2>/dev/null); T0=$(python3 -c 'import time;print(time.time())')
wait_for_line $OUT/app.log "WCSTAT vdo" 900 $LP   # up to 90 s
C1=$(ps -o time= -p ${PID:-0} 2>/dev/null); T1=$(python3 -c 'import time;print(time.time())')
python3 - "$C0" "$C1" "$T0" "$T1" > $OUT/cpu.txt <<'P'
import sys
def s(t):
    t = t.strip(); p = t.split(':')
    return float(p[0]) * 60 + float(p[1]) if len(p) == 2 else float(p[0]) * 3600 + float(p[1]) * 60 + float(p[2])
try: print("appCPU=%.1f%%" % ((s(sys.argv[2]) - s(sys.argv[1])) / (float(sys.argv[4]) - float(sys.argv[3])) * 100))
except Exception: print("appCPU=?")
P
wait_for_line $OUT/app.log DONE 600 $LP   # up to 60 s
sleep 0.3; kill $LP 2>/dev/null
BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null); [[ -f $BL/dylib-$PID.log ]] && cp $BL/dylib-$PID.log $OUT/dylib.log
xcrun simctl terminate $U $B 2>/dev/null
echo "== $RT $TAG pid=$PID $(cat $OUT/cpu.txt) done=$(grep -c DONE $OUT/app.log) fail=$(grep -c '| FAIL' $OUT/app.log)"
grep -hE "WCSTAT|\| FAIL" $OUT/app.log | sed 's/path=.*//' | cut -c8-420
grep -hE "webcam: (live|offline|ring /|pump)" $OUT/dylib.log 2>/dev/null | head -12 | cut -c20-230
grep -h "webcam stat" $OUT/dylib.log 2>/dev/null | cut -c20-330
