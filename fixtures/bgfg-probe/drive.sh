#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# drive.sh <rt: 263|27> <mode> <tag> <inj: boot|none> — one bgfg-probe run
#   mode: stopInBackground | keepRunning | stopWhileInterrupted (the probe's -mode argument; README.md)
#   inj=boot: the sim was booted by scripts/boot.sh <rt> stub (engine log copied from the boot log dir); none: booted plainly
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to the launch (e.g. XENV=M12_NO_INTERRUPT=1); BGS = seconds in the background (3)
#   UDID: $SIMCAM_UDID_<rt>. Writes $SIMCAM_RESULTS/bgfg-<rt>-<inj>-<mode>-<tag>/: app.log, host.log, dylib.log, timeline.txt
# Backgrounding: `simctl launch <udid> com.apple.Preferences`, then after BGS s `simctl launch <udid> <probe>` (as run.sh does
# for the canary's BG-REQUEST); the second launch brings the running probe back to the foreground, it doesn't relaunch it.
set -u; zmodload zsh/datetime
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; MODE=$2; TAG=$3; INJ=$4
V=SIMCAM_UDID_$RT; UDID=${(P)V:-}; [[ -n $UDID ]] || { echo "$0: set $V" >&2; exit 2; }
B=dev.simcam.e20.bgfgprobe; OUT=$SIMCAM_RESULTS/bgfg-$RT-$INJ-$MODE-$TAG; rm -rf $OUT; mkdir -p $OUT
hlog() { printf 'HOST| %.3f %s\n' $(( EPOCHREALTIME - 978307200 )) "$*" >> $OUT/host.log; }
typeset -a ENVS; ENVS=(); for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done
xcrun simctl terminate $UDID $B 2>/dev/null
xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
hlog "launch $B -mode $MODE ${XENV:-}"
env $ENVS xcrun simctl launch --console-pty $UDID $B -mode $MODE > $OUT/app.log 2>&1 &
LP=$!; BG=0
for i in {1..450}; do sleep 0.1
  grep -q "END |" $OUT/app.log && break
  kill -0 $LP 2>/dev/null || { hlog "console closed (app gone)"; break; }
  if [[ $BG == 0 ]] && grep -q "BG-REQUEST" $OUT/app.log; then BG=1
    hlog "simctl launch com.apple.Preferences"; xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1; hlog "simctl launch com.apple.Preferences returned"
    sleep ${BGS:-3}
    hlog "simctl launch $B (foreground)"; xcrun simctl launch $UDID $B >/dev/null 2>&1; hlog "simctl launch $B returned"
  fi
done
grep -q "END |" $OUT/app.log || hlog "no END after 45 s"
# is the engine still pumping after the app's last stop? CPU time of the probe over 3 s, and a 2 s `sample` of it
# (stacks in libsimcam.dylib's pump; the app's own code is idle by then)
PID=$(grep -oE 'pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2)
if [[ -n $PID ]] && kill -0 $PID 2>/dev/null; then
  cpu() { ps -o time= -p $PID | awk -F'[:.]' '{print ($1*60+$2)+("0."$3)}'; }
  C1=$(cpu); sleep 3; C2=$(cpu); hlog "after END: probe CPU $(( C2 - C1 )) s in 3 s"
  /usr/bin/sample $PID 2 -file $OUT/sample.txt >/dev/null 2>&1
  hlog "after END: sample: $(grep -c 'libsimcam.dylib' $OUT/sample.txt 2>/dev/null) stack lines in libsimcam.dylib, $(grep -E "M5StartPump|M5Pump" $OUT/sample.txt 2>/dev/null | grep -c .) naming the pump"
fi
sleep 0.1; kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null; hlog "terminated"
if [[ $INJ == boot ]]; then BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)
  if [[ -n $PID && -f $BL/dylib-$PID.log ]]; then cp $BL/dylib-$PID.log $OUT/dylib.log; else echo "no engine log (pid ${PID:-?})" > $OUT/dylib.log; fi
fi
python3 -I ${0:a:h}/merge.py $OUT > $OUT/timeline.txt
echo "$RT/$INJ/$MODE/$TAG ${XENV:-} :: pid=${PID:-?} $(grep -c . $OUT/timeline.txt) lines, END=$(grep -c 'END |' $OUT/app.log)"
