#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# drive.sh <rt: 263|27> <inj: boot|none> <tag> read | feature <feature> [camera] — one features-probe launch (w4 e22)
#   XENV="VAR=val ..." per-launch engine settings; MOVIE_SECONDS=<s> movie length (w4 e23b)
#   inj=boot: the sim was booted by scripts/boot.sh <rt> stub (engine log copied from the boot log dir); none: booted plainly
#   UDID: $SIMCAM_UDID_<rt>. Writes $SIMCAM_RESULTS/features-<rt>-<inj>-<what>-<tag>/: app.log, host.log, dylib.log
#   Waits for END (90 s at most), or for the app to go away (crash: the console closes; host.log says so).
set -u; zmodload zsh/datetime
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; INJ=$2; TAG=$3; MODE=$4; FEAT=${5:-}; CAM=${6:-back-wide}; NOCATCH=${NOCATCH:-0}
V=SIMCAM_UDID_$RT; UDID=${(P)V:-}; [[ -n $UDID ]] || { echo "$0: set $V" >&2; exit 2; }
B=dev.simcam.e22.featuresprobe; WHAT=$MODE; [[ $MODE == feature ]] && WHAT=$FEAT-$CAM; [[ $NOCATCH == 1 ]] && WHAT=$WHAT-nocatch
OUT=$SIMCAM_RESULTS/features-$RT-$INJ-$WHAT-$TAG; rm -rf $OUT; mkdir -p $OUT
hlog() { printf 'HOST| %.3f %s\n' $(( EPOCHREALTIME - 978307200 )) "$*" >> $OUT/host.log; }
typeset -a ARGS; ARGS=(-mode $MODE); [[ $MODE == feature ]] && ARGS+=(-feature $FEAT -camera $CAM -nocatch $NOCATCH)
[[ -n ${MOVIE_SECONDS:-} ]] && ARGS+=(-seconds $MOVIE_SECONDS)   # w4 e23b: movie length (default 3 s)
# w4 e23b: XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to the launch, as scripts/run.sh does (e.g. XENV=M15_OLD_BRACKET=1)
typeset -a ENVS; ENVS=(); for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done
xcrun simctl terminate $UDID $B 2>/dev/null
xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
hlog "launch $B $ARGS${XENV:+ (XENV=$XENV)}"
env $ENVS xcrun simctl launch --console-pty $UDID $B $ARGS > $OUT/app.log 2>&1 &
LP=$!; GONE=0
for i in {1..900}; do sleep 0.1
  grep -qaE "^PROBE\| [0-9.]+ +[0-9.]+ END" $OUT/app.log && break
  kill -0 $LP 2>/dev/null || { GONE=1; hlog "console closed before END (app gone)"; break; }
done
grep -qaE "^PROBE\| [0-9.]+ +[0-9.]+ END" $OUT/app.log || { [[ $GONE == 1 ]] || hlog "no END after 90 s"; }
PID=$(grep -oE 'pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2)
sleep 0.2; kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null; hlog "terminated"
if [[ $INJ == boot ]]; then BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)
  if [[ -n $PID && -f $BL/dylib-$PID.log ]]; then cp $BL/dylib-$PID.log $OUT/dylib.log; else echo "no engine log (pid ${PID:-?})" > $OUT/dylib.log; fi
fi
echo "$RT/$INJ/$WHAT/$TAG pid=${PID:-?} END=$(grep -caE "^PROBE\| [0-9.]+ +[0-9.]+ END" $OUT/app.log) gone=$GONE :: $(grep -ao 'RESULT feature=.*' $OUT/app.log | head -1)"
