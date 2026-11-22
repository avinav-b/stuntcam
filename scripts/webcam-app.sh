#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# webcam-app.sh <rt> <tag> [K=V ...] — one run of fixtures/plain in M5APP_MODE=webcam on the sim $SIMCAM_UDID_<rt>
# (armed with scripts/boot.sh <rt> stub). The source is $SRC (default webcam:synthetic). It is set through a per-run
# control file, so a test can switch it mid-run (source=...). $ORIENT adds orientation=$ORIENT. Extra K=V go to the app
# (WC_SECS, WC_MOVIE, WC_PHOTO, WC_W/WC_H, ...).
# Writes $SIMCAM_RESULTS/webcam-<rt>-<tag>/: app.log, dylib.log, control.txt and cpu.txt (the app's CPU % over the
# measured window, from wc.running + 1 s to the VDO WCSTAT line). Prints the WCSTAT lines and the engine's webcam lines.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=$1
TAG=$2
shift 2
need_udid "$0" "$RUNTIME" UDID short

OUT=$SIMCAM_RESULTS/webcam-$RUNTIME-$TAG
rm -rf $OUT
mkdir -p $OUT

CONTROL=$OUT/control.txt
print -r -- "source=${SRC:-webcam:synthetic}" > $CONTROL
[[ -n ${ORIENT:-} ]] && print -r -- "orientation=$ORIENT" >> $CONTROL

BUNDLE=dev.simcam.spike.m10plain   # fixtures/plain (a historical id, kept: the app uses it)
typeset -a ENVS
ENVS=(SIMCTL_CHILD_M5APP_MODE=webcam "SIMCTL_CHILD_SIMCAM_CONTROL=$CONTROL")
for kv in "$@"; do
  ENVS+=("SIMCTL_CHILD_$kv")
done

# wall_clock: the Unix time in seconds, with a fraction.
wall_clock() { python3 -c 'import time;print(time.time())'; }

xcrun simctl terminate $UDID $BUNDLE 2>/dev/null   # not running: an expected error
xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null
env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE > $OUT/app.log 2>&1 &
LAUNCH_PID=$!

# Waits up to 10 s for the engine's first line (it holds the app's pid), then up to 15 s for the app's wc.running,
# and 1 s more.
PID=
for i in {1..100}; do
  sleep 0.1
  PID=$(grep -m1 -oE '\[simcam [0-9]+\]' $OUT/app.log | grep -oE '[0-9]+')
  [[ -n $PID ]] && break
done

for i in {1..150}; do
  sleep 0.1
  grep -q "wc.running" $OUT/app.log && break
done

sleep 1
# a pid that is gone gives no time: the cpu line then says so
CPU_START=$(ps -o time= -p ${PID:-0} 2>/dev/null)
WALL_START=$(wall_clock)
wait_for_line $OUT/app.log "WCSTAT vdo" 900 $LAUNCH_PID   # up to 90 s
CPU_END=$(ps -o time= -p ${PID:-0} 2>/dev/null)
WALL_END=$(wall_clock)

python3 - "$CPU_START" "$CPU_END" "$WALL_START" "$WALL_END" > $OUT/cpu.txt <<'PY'
import sys


def cpu_seconds(ps_time):
    """Returns the seconds in a ps TIME field: "M:SS.ss" or "H:MM:SS.ss"."""
    fields = ps_time.strip().split(':')
    if len(fields) == 2:
        return float(fields[0]) * 60 + float(fields[1])
    return float(fields[0]) * 3600 + float(fields[1]) * 60 + float(fields[2])


try:
    cpu_start, cpu_end, wall_start, wall_end = sys.argv[1:5]
    cpu_used = cpu_seconds(cpu_end) - cpu_seconds(cpu_start)
    elapsed = float(wall_end) - float(wall_start)
    print("appCPU=%.1f%%" % (cpu_used / elapsed * 100))
except Exception:
    print("appCPU=?")
PY

wait_for_line $OUT/app.log DONE 600 $LAUNCH_PID   # up to 60 s
sleep 0.3
# whatever already ended: nothing to report
kill $LAUNCH_PID 2>/dev/null

# no file: no engine log to copy, and the dylib.log lines below print nothing
BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)
[[ -f $BOOT_LOG_DIR/dylib-$PID.log ]] && cp $BOOT_LOG_DIR/dylib-$PID.log $OUT/dylib.log
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null

CPU_TEXT=$(cat $OUT/cpu.txt)
DONE_LINES=$(grep -c DONE $OUT/app.log)
FAIL_LINES=$(grep -c '| FAIL' $OUT/app.log)
echo "== $RUNTIME $TAG pid=$PID $CPU_TEXT done=$DONE_LINES fail=$FAIL_LINES"
grep -hE "WCSTAT|\| FAIL" $OUT/app.log | sed 's/path=.*//' | cut -c8-420
grep -hE "webcam: (live|offline|ring /|pump)" $OUT/dylib.log 2>/dev/null | head -12 | cut -c20-230
grep -h "webcam stat" $OUT/dylib.log 2>/dev/null | cut -c20-330
