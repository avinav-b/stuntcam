#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# drive.sh <rt: 263|27|175> <tag> <inj: boot|none> [probe args...] — one motion-probe launch
#   probe args: -camera 0|1 -order motionFirst|cameraFirst [-dur s] [-hz Hz] (README.md)
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to the launch (SIMCAM_LAZY=0: eager; SIMCAM=0: unarmed); one VAR=val per line
#   when a value holds spaces
#   UDID: $SIMCAM_UDID_<rt>. Writes $SIMCAM_RESULTS/motion-<rt>-<tag>/: app.log, dylib.log, args.txt
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; TAG=$2; INJ=$3; shift 3
V=SIMCAM_UDID_$RT; UDID=${(P)V:-}; [[ -n $UDID ]] || { echo "$0: set $V" >&2; exit 2; }
B=dev.simcam.e24.motionprobe; OUT=$SIMCAM_RESULTS/motion-$RT-$TAG; rm -rf $OUT; mkdir -p $OUT
# XENV: K=V words split on whitespace, or one K=V per line when it holds a newline (values with spaces), as in run.sh
typeset -a ENVS; ENVS=()
if [[ ${XENV:-} == *$'\n'* ]]; then for kv in "${(@f)XENV}"; do [[ -n $kv ]] && ENVS+=("SIMCTL_CHILD_$kv"); done
else for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done; fi
echo "rt=$RT inj=$INJ xenv=${XENV:-} args=$*" > $OUT/args.txt
xcrun simctl terminate $UDID $B 2>/dev/null
xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant motion $B 2>/dev/null
env $ENVS xcrun simctl launch --console-pty $UDID $B "$@" > $OUT/app.log 2>&1 &
LP=$!
for i in {1..400}; do sleep 0.1; grep -q "PROBE|.* END" $OUT/app.log && break; kill -0 $LP 2>/dev/null || break; done
grep -q "PROBE|.* END" $OUT/app.log || echo "HOST| no END after 40 s" >> $OUT/app.log
sleep 0.2; kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null
PID=$(grep -oE 'pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2)
if [[ $INJ == boot ]]; then BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)
  if [[ -n $PID && -f $BL/dylib-$PID.log ]]; then cp $BL/dylib-$PID.log $OUT/dylib.log; else echo "no engine log (pid ${PID:-?}): not armed" > $OUT/dylib.log; fi
fi
echo "$RT/$TAG :: pid=${PID:-?} $(grep -E 'MGR [AB]' $OUT/app.log | grep -oE 'MGR [AB] count=[0-9]+' | tr '\n' ' ') END=$(grep -c 'PROBE|.* END' $OUT/app.log)"
