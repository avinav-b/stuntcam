#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# bgfg.sh <rt> [behaviour ...] [-n N] — does the engine stay in step with the app's own startRunning/stopRunning across
#   background and foreground? [L52]
#   On a sim armed by scripts/boot.sh <rt> stub, the bg/fg probe (fixtures/bgfg-probe, `-mode <behaviour>`) runs N times
#   (default 1) per behaviour (default all: stopInBackground keepRunning stopWhileInterrupted appNotif; see its README).
#   Each run launches the probe, takes it to the background at its BG-REQUEST (`simctl launch com.apple.Preferences`) and
#   back after BGS s (default 6: on 27.0 didEnterBackground came 2.2–4.8 s after Preferences launched) with
#   `simctl launch <probe>`, waits for its END, and checks the probe log against the engine log (dylib-<pid>.log):
#     (a) every startRunning/stopRunning the probe makes reaches the engine: matched in order, each CALL to an engine
#         "session … StartRunning" / "StopRunning" line logged between that CALL and its RET (or the next CALL);
#     (b) the engine never restarts a session the app stopped: no "interruption ended session …" whose last app call
#         before it was a stopRunning;
#     (c) after the probe's final stopRunning the engine has stopped that session: the session's last engine Start/Stop/
#         interruption-ended line is the StopRunning that call made, and no "interruption ended" follows it.
#   Also required: the probe reached END, was backgrounded and foregrounded (didEnterBackground, willEnterForeground), and
#   left an engine log; for stopWhileInterrupted and appNotif, that an app stop reached the engine while it had the session
#   interrupted (else the run didn't exercise L52: "not exercised"); no more redundant calls than the behaviour makes; and
#   the probe's CPU over 2 s after END under 0.1 s (an engine still pumping uses 0.28-0.36 s; stopped: <= 0.01 s).
#   The foreground comes BGS s after the background, and for those two behaviours not before the probe's background stop
#   has returned (up to 15 s more), so a slow didEnterBackground can't put the stop after the interruption's end.
#   Prints one PASS/FAIL line per behaviour and repetition with the reason; exits 1 if any failed (2 on a usage error).
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to the probe launch, as scripts/run.sh does (e.g. XENV=SIMCAM_OFF_INTERRUPTION_RESET=1).
#   UDID: $SIMCAM_UDID_<rt>. Needs fixtures/bgfg-probe/build/bgfgprobe.app (fixtures/bgfg-probe/build.sh); installed here.
#   Writes $SIMCAM_RESULTS/bgfg-<rt>-<behaviour>-<tag>-r<i>/: app.log, host.log, dylib.log, timeline.txt, check.txt.
set -u; zmodload zsh/datetime
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh
(( $# )) || { echo "usage: $0 <rt> [behaviour ...] [-n N]" >&2; exit 2; }
RT=$1; shift; N=1; typeset -a MODES; MODES=()
while (( $# )); do case $1 in -n) (( $# > 1 )) || { echo "$0: -n needs a count" >&2; exit 2; }; N=$2; shift 2;; -n*) N=${1#-n}; shift;; *) MODES+=($1); shift;; esac; done
[[ $N == <1-> ]] || { echo "$0: -n needs a positive count (got $N)" >&2; exit 2; }
(( ${#MODES} )) || MODES=(stopInBackground keepRunning stopWhileInterrupted appNotif)
for m in $MODES; do [[ $m == (stopInBackground|keepRunning|stopWhileInterrupted|appNotif) ]] || { echo "$0: unknown behaviour $m" >&2; exit 2; }; done
need_udid "$0" "$RT" UDID long
BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null); [[ -n $BL && -d $BL ]] ||
  { echo "$0: no boot log dir: boot with scripts/boot.sh $RT stub first" >&2; exit 2; }
B=dev.simcam.e20.bgfgprobe; APP=$ROOT/fixtures/bgfg-probe/build/bgfgprobe.app; BGS=${BGS:-6}; TAG=bgfg-$(date +%H%M%S)
[[ -d $APP ]] || { echo "$0: $APP missing: run fixtures/bgfg-probe/build.sh" >&2; exit 2; }
xcrun simctl install $UDID $APP || { echo "$0: installing the probe failed" >&2; exit 2; }
typeset -a ENVS; ENVS=(); for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done

hlog() { printf 'HOST| %.3f %s\n' $(( EPOCHREALTIME - 978307200 )) "$*" >> $OUT/host.log; }
fails=0; total=0
for m in $MODES; do for i in {1..$N}; do
  OUT=$SIMCAM_RESULTS/bgfg-$RT-$m-$TAG-r$i; rm -rf $OUT; mkdir -p $OUT; total=$((total + 1))
  xcrun simctl terminate $UDID $B 2>/dev/null
  xcrun simctl privacy $UDID grant camera $B 2>/dev/null; xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
  hlog "launch $B -mode $m ${XENV:-}"
  env $ENVS xcrun simctl launch --console-pty $UDID $B -mode $m > $OUT/app.log 2>&1 &
  LP=$!; BG=0
  for k in {1..600}; do sleep 0.1   # up to 60 s for the probe's END
    grep -q "END |" $OUT/app.log && break
    kill -0 $LP 2>/dev/null || { hlog "console closed (app gone)"; break; }
    if [[ $BG == 0 ]] && grep -q "BG-REQUEST" $OUT/app.log; then BG=1
      hlog "simctl launch com.apple.Preferences"; xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1
      sleep $BGS
      if [[ $m == (stopWhileInterrupted|appNotif) ]]; then   # not before the background stop has returned (up to 15 s more)
        for w in {1..150}; do awk '/SCENE didEnterBackground/{b=1} b&&/RET  stopRunning/{f=1} END{exit !f}' $OUT/app.log && break; sleep 0.1; done
      fi
      hlog "simctl launch $B (foreground)"; xcrun simctl launch $UDID $B >/dev/null 2>&1
    fi
  done
  grep -q "END |" $OUT/app.log || hlog "no END after 60 s"
  # a late restart or pump shows after END: wait 2 s (the engine log keeps it), and read the probe's CPU over that time
  PID=$(grep -oE 'pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2); CPU=?
  if [[ -n $PID ]] && kill -0 $PID 2>/dev/null; then
    cpu() { ps -o time= -p $PID | awk -F'[:.]' '{print ($1*60+$2)+("0."$3)}'; }
    C1=$(cpu); sleep 2; C2=$(cpu); [[ -n $C1 && -n $C2 ]] && CPU=$(printf '%.2f' $(( C2 - C1 ))); hlog "after END: probe CPU $CPU s in 2 s"
  else sleep 2; fi
  kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null; hlog "terminated"
  if [[ -n $PID && -f $BL/dylib-$PID.log ]]; then cp $BL/dylib-$PID.log $OUT/dylib.log; else : > $OUT/dylib.log; fi
  python3 -I $ROOT/fixtures/bgfg-probe/merge.py $OUT > $OUT/timeline.txt 2>/dev/null
  # the three assertions and the other requirements (scripts/bgfg-check.py): "PASS <info>" or "FAIL <reason>"
  R=$(python3 -I $ROOT/scripts/bgfg-check.py $OUT $m 2>&1 | tail -1); [[ $R == PASS* || $R == FAIL* ]] || R="FAIL checker: $R"
  if [[ $CPU != '?' ]] && (( CPU > 0.1 )); then   # the engine still pumping after the app's last stop (L52's symptom)
    [[ $R == PASS* ]] && R="FAIL probe CPU ${CPU} s in 2 s after END (engine still pumping?) | ${R#PASS }" || R="$R; probe CPU ${CPU} s after END"
  fi
  echo "$R" > $OUT/check.txt
  [[ $R == PASS* ]] || fails=$((fails + 1))
  printf '%s %s %s r%d%s :: %s cpu-after-END=%ss\n' ${R%% *} $RT $m $i "${XENV:+ ($XENV)}" "${R#* }" $CPU
done; done
echo "bgfg $RT: $((total - fails))/$total PASS${XENV:+ ($XENV)} (results: $SIMCAM_RESULTS/bgfg-$RT-*-$TAG-r*)"
(( fails )) && exit 1 || exit 0
