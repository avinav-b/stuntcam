#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# bgfg.sh <rt> [behaviour ...] [-n N] — does the engine stay in step with the app's own startRunning/stopRunning across
#   background and foreground?
#   On a sim armed by scripts/boot.sh <rt> stub, the bg/fg probe (fixtures/bgfg-probe, `-mode <behaviour>`) runs N times
#   (default 1) per behaviour (default all: stopInBackground keepRunning stopWhileInterrupted appNotif; see its README).
#   Each run launches the probe, takes it to the background at its BG-REQUEST (`simctl launch com.apple.Preferences`)
#   and back after BGS s (default 6: on 27.0 didEnterBackground came 2.2–4.8 s after Preferences launched) with `simctl
#   launch <probe>`, waits for its END, and checks the probe log against the engine log (dylib-<pid>.log):
#     (a) every startRunning/stopRunning the probe makes reaches the engine: matched in order, each CALL to an engine
#         "session … StartRunning" / "StopRunning" line logged between that CALL and its RET (or the next CALL);
#     (b) the engine never restarts a session the app stopped: no "interruption ended session …" whose last app call
#         before it was a stopRunning;
#     (c) after the probe's final stopRunning the engine has stopped that session: the session's last engine Start/Stop/
#         interruption-ended line is the StopRunning that call made, and no "interruption ended" follows it.
#   Also required: the probe reached END, was backgrounded and foregrounded (didEnterBackground, willEnterForeground),
#   and left an engine log; for stopWhileInterrupted and appNotif, that an app stop reached the engine while it had the
#   session interrupted (else the run didn't exercise a stop while interrupted: "not exercised"); no more redundant
#   calls than the behaviour makes; and the probe's CPU over 2 s after END under 0.1 s (an engine still pumping uses
#   0.28-0.36 s; stopped: <= 0.01 s).
#   The foreground comes BGS s after the background, and for those two behaviours not before the probe's background stop
#   has returned (up to 15 s more), so a slow didEnterBackground can't put the stop after the interruption's end.
#   Prints one PASS/FAIL line per behaviour and repetition with the reason; exits 1 if any failed (2 on a usage error).
#   XENV="VAR=val ..." adds SIMCTL_CHILD_VAR=val to the probe launch, as scripts/run.sh does
#   (e.g. XENV=SIMCAM_OFF_INTERRUPTION_RESET=1).
#   UDID: $SIMCAM_UDID_<rt>. Needs fixtures/bgfg-probe/build/bgfgprobe.app
#   (fixtures/bgfg-probe/build.sh); installed here.
#   Writes $SIMCAM_RESULTS/bgfg-<rt>-<behaviour>-<tag>-r<i>/: app.log, host.log, dylib.log, timeline.txt, check.txt.
set -u
zmodload zsh/datetime
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

(( $# )) || { echo "usage: $0 <rt> [behaviour ...] [-n N]" >&2; exit 2; }
RUNTIME=$1
shift

N=1
typeset -a MODES
MODES=()
while (( $# )); do
  case $1 in
    -n)
      (( $# > 1 )) || { echo "$0: -n needs a count" >&2; exit 2; }
      N=$2
      shift 2
      ;;
    -n*)
      N=${1#-n}
      shift
      ;;
    *)
      MODES+=($1)
      shift
      ;;
  esac
done

[[ $N == <1-> ]] || { echo "$0: -n needs a positive count (got $N)" >&2; exit 2; }
(( ${#MODES} )) || MODES=(stopInBackground keepRunning stopWhileInterrupted appNotif)
for m in $MODES; do
  [[ $m == (stopInBackground|keepRunning|stopWhileInterrupted|appNotif) ]] ||
    { echo "$0: unknown behaviour $m" >&2; exit 2; }
done

need_udid "$0" "$RUNTIME" UDID long
BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)   # no file: checked next
[[ -n $BOOT_LOG_DIR && -d $BOOT_LOG_DIR ]] ||
  { echo "$0: no boot log dir: boot with scripts/boot.sh $RUNTIME stub first" >&2; exit 2; }

BUNDLE=dev.simcam.e20.bgfgprobe
APP=$ROOT/fixtures/bgfg-probe/build/bgfgprobe.app
BGS=${BGS:-6}
TAG=bgfg-$(date +%H%M%S)
[[ -d $APP ]] || { echo "$0: $APP missing: run fixtures/bgfg-probe/build.sh" >&2; exit 2; }
xcrun simctl install $UDID $APP || { echo "$0: installing the probe failed" >&2; exit 2; }

typeset -a ENVS
ENVS=()
for kv in ${=XENV:-}; do
  ENVS+=(SIMCTL_CHILD_$kv)
done

# host_log <text>: writes a host line to the run's host.log, stamped in CFAbsoluteTime (seconds since 2001,
# 978307200 s after the epoch) like the probe's own lines.
host_log() { printf 'HOST| %.3f %s\n' $(( EPOCHREALTIME - 978307200 )) "$*" >> $OUT/host.log; }

# background_stop_returned: whether the probe's log has a stopRunning return after its didEnterBackground.
background_stop_returned() {
  awk '/SCENE didEnterBackground/ { backgrounded = 1 }
    backgrounded && /RET  stopRunning/ { returned = 1 }
    END { exit !returned }' $OUT/app.log
}

# probe_cpu_s: the probe's CPU time so far, in seconds (ps prints M:SS.ss).
probe_cpu_s() { ps -o time= -p $PID | awk -F'[:.]' '{print ($1*60+$2)+("0."$3)}'; }

fails=0
total=0
for m in $MODES; do
  for i in {1..$N}; do
    OUT=$SIMCAM_RESULTS/bgfg-$RUNTIME-$m-$TAG-r$i
    rm -rf $OUT
    mkdir -p $OUT
    total=$((total + 1))

    # not running / already granted: nothing to report
    xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
    xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
    xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null

    host_log "launch $BUNDLE -mode $m ${XENV:-}"
    env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE -mode $m > $OUT/app.log 2>&1 &
    LAUNCH_PID=$!

    BACKGROUNDED=0
    for k in {1..600}; do   # up to 60 s (0.1 s polls) for the probe's END
      sleep 0.1
      grep -q "END |" $OUT/app.log && break
      if ! kill -0 $LAUNCH_PID 2>/dev/null; then
        host_log "console closed (app gone)"
        break
      fi
      if [[ $BACKGROUNDED == 0 ]] && grep -q "BG-REQUEST" $OUT/app.log; then
        BACKGROUNDED=1
        host_log "simctl launch com.apple.Preferences"
        # the background trip: a launch that fails shows in the probe's timeline
        xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1
        sleep $BGS
        # For these two, not before the background stop has returned (up to 15 s more).
        if [[ $m == (stopWhileInterrupted|appNotif) ]]; then
          for w in {1..150}; do
            background_stop_returned && break
            sleep 0.1
          done
        fi
        host_log "simctl launch $BUNDLE (foreground)"
        xcrun simctl launch $UDID $BUNDLE >/dev/null 2>&1
      fi
    done
    grep -q "END |" $OUT/app.log || host_log "no END after 60 s"

    # A late restart or pump shows after END. So this waits 2 s (the engine log keeps it) and reads the probe's CPU
    # over that time.
    PID=$(grep -oE 'pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2)
    CPU=?
    if [[ -n $PID ]] && kill -0 $PID 2>/dev/null; then
      CPU_BEFORE=$(probe_cpu_s)
      sleep 2
      CPU_AFTER=$(probe_cpu_s)
      [[ -n $CPU_BEFORE && -n $CPU_AFTER ]] && CPU=$(printf '%.2f' $(( CPU_AFTER - CPU_BEFORE )))
      host_log "after END: probe CPU $CPU s in 2 s"
    else
      sleep 2
    fi

    # whatever already ended: nothing to report
    kill $LAUNCH_PID 2>/dev/null
    xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
    xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null
    host_log "terminated"

    if [[ -n $PID && -f $BOOT_LOG_DIR/dylib-$PID.log ]]; then
      cp $BOOT_LOG_DIR/dylib-$PID.log $OUT/dylib.log
    else
      : > $OUT/dylib.log
    fi

    # the timeline is evidence; bgfg-check.py below gives the verdict
    python3 -I $ROOT/fixtures/bgfg-probe/merge.py $OUT > $OUT/timeline.txt 2>/dev/null
    # The three assertions and the other requirements (scripts/bgfg-check.py): "PASS <info>" or "FAIL <reason>".
    RESULT=$(python3 -I $ROOT/scripts/bgfg-check.py $OUT $m 2>&1 | tail -1)
    [[ $RESULT == PASS* || $RESULT == FAIL* ]] || RESULT="FAIL checker: $RESULT"
    if [[ $CPU != '?' ]] && (( CPU > 0.1 )); then   # the engine still pumping after the app's last stop
      if [[ $RESULT == PASS* ]]; then
        RESULT="FAIL probe CPU ${CPU} s in 2 s after END (engine still pumping?) | ${RESULT#PASS }"
      else
        RESULT="$RESULT; probe CPU ${CPU} s after END"
      fi
    fi

    echo "$RESULT" > $OUT/check.txt
    [[ $RESULT == PASS* ]] || fails=$((fails + 1))
    printf '%s %s %s r%d%s :: %s cpu-after-END=%ss\n' ${RESULT%% *} $RUNTIME $m $i "${XENV:+ ($XENV)}" \
      "${RESULT#* }" $CPU
  done
done

echo "bgfg $RUNTIME: $((total - fails))/$total PASS${XENV:+ ($XENV)} (results: $SIMCAM_RESULTS/bgfg-$RUNTIME-*-$TAG-r*)"
if (( fails )); then
  exit 1
fi
exit 0
