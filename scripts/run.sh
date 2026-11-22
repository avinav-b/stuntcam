#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# run.sh <app> <rt: 263|27|175> <inj: boot|launchd|launch|none> <tag>
#   app: canary | vc | vc-audio | vcfull | vcfull-audio | expo | expo-audio | vcqr | expoqr | flutter (all tests) |
#        flutter-<test>
#   inj=boot   : the sim was booted by boot.sh with the stub; nothing per-launch (boot env wins)
#   inj=launchd: the same for a sim armed in launchd mode (boot.sh <rt> launchd, or launchd-arm.sh); nothing per-launch
#   inj=launch : per-launch SIMCTL_CHILD_DYLD_INSERT_LIBRARIES=<real dylib> + config (sim booted plainly)
#   UDID: $SIMCAM_UDID_<rt> (e.g. SIMCAM_UDID_263). Metro: $EXPO_PORT (vc*, expo*; default 8099)
# Writes results/<app>-<rt>-<inj>-<tag>/ : app.log, dylib.log, summary.txt (PASS/FAIL counts), screenshots, .ips
#   media checks use tools/bin/avinfo and tools/bin/avaudio (build them with tools/build.sh)
#   WAIT_DEBUGGER=1 (plain only): launch with simctl --wait-for-debugger; the caller attaches (scripts/lldb-attach.py)
#   NO_MIC_GRANT=1: don't grant the microphone before launch (camera is still granted), so a revoked or reset (not
#   determined) mic permission set with `xcrun simctl privacy <udid> revoke|reset microphone <bundle>` stays as it is
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

APP_NAME=$1
RUNTIME=$2
INJECTION=$3
TAG=${4:-x}

need_udid "$0" "$RUNTIME" UDID long

OUT=$SIMCAM_RESULTS/$APP_NAME-$RUNTIME-$INJECTION-$TAG
rm -rf $OUT
mkdir -p $OUT

ENGINE=${DYLIB:-$(cat $ROOT/build/current_dylib.txt)}
PROFILE=${PLIST:-$SIMCAM_DEV/profiles/cfg-m10.plist}

typeset -a ENVS ARGS
ENVS=()
ARGS=()

EXPO_PORT=${EXPO_PORT:-8099}
PLAIN_WAIT_POLLS=300         # plain's DONE line: 0.1 s polls (30 s)
CANARY_LAUNCH_SLACK_S=15     # the canary's own budget plus this long for the launch
APP_START_GRACE_S=6          # the other apps: "APP GONE" is only checked after this many 1 s polls

# app_running: true while launchd still lists the app ($BUNDLE)
app_running() { xcrun simctl spawn $UDID launchctl list 2>/dev/null | grep -q "UIKitApplication:$BUNDLE"; }

# the app's bundle id, launch arguments, log file name and end marker; MAXW: 1 s polls for the end marker. The m10, m1b
# and m7 in these names (and in cfg-m10.plist above) are the milestones that added each app; they are data and stay.
APP_LOG_NAME=m1b.log
LOG_MARK=M1B
END_PATTERN="END "
MAXW=${MAXW:-}
case $APP_NAME in
  canary) BUNDLE=dev.simcam.spike.m10canary ;;
  plain) BUNDLE=dev.simcam.spike.m10plain ;;
  vc|expo)
    BUNDLE=dev.spike.m1bcam
    ARGS=(-CAMTEST $APP_NAME -RCT_jsLocation localhost:$EXPO_PORT)
    ;;
  vc-audio|expo-audio)
    BUNDLE=dev.spike.m1bcam
    ARGS=(-CAMTEST ${APP_NAME%-audio} -AUDIO 1 -RCT_jsLocation localhost:$EXPO_PORT)
    ;;
  vcqr|expoqr)   # scripts/qr-test.sh
    BUNDLE=dev.spike.m1bcam
    ARGS=(-CAMTEST $APP_NAME -QRPOS ${QRPOS:-back} -QRSECS ${QRSECS:-10} -RCT_jsLocation localhost:$EXPO_PORT)
    ;;
  vcfull)   # exhaustive VisionCamera checks (formats, photos, recordings); 4K encodes are slow
    BUNDLE=dev.spike.m1bcam
    ARGS=(-CAMTEST vcfull -RCT_jsLocation localhost:$EXPO_PORT)
    : ${MAXW:=300}
    ;;
  vcfull-audio)
    BUNDLE=dev.spike.m1bcam
    ARGS=(-CAMTEST vcfull -AUDIO 1 -RCT_jsLocation localhost:$EXPO_PORT)
    : ${MAXW:=300}
    ;;
  flutter*)
    BUNDLE=dev.spike.m7cam
    APP_LOG_NAME=m7.log
    LOG_MARK=M7
    END_PATTERN="M7| END$"
    FLUTTER_TEST=${APP_NAME#flutter}
    FLUTTER_TEST=${FLUTTER_TEST#-}
    FLUTTER_TEST=${FLUTTER_TEST:-all}
    ;;
esac
MAXW=${MAXW:-150}

# not running / already granted: nothing to report
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
[[ ${NO_MIC_GRANT:-0} == 1 ]] || xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null

if [[ $INJECTION == launch ]]; then
  inject_env "$ENGINE" "$PROFILE" "$OUT/dylib.log"
  ENVS=($INJECT_ENV SIMCTL_CHILD_SIMCAM_FIXTURE_IMAGE=$SIMCAM_DEV/media/qr.png
    SIMCTL_CHILD_M5_IMAGE=$SIMCAM_DEV/media/qr.png)
fi

# XENV: K=V words split on whitespace, or one K=V per line when it holds a newline
# (values with spaces, e.g. paths under "Application Support")
if [[ ${XENV:-} == *$'\n'* ]]; then
  for kv in "${(@f)XENV}"; do
    [[ -n $kv ]] && ENVS+=("SIMCTL_CHILD_$kv")
  done
else
  for kv in ${=XENV:-}; do
    ENVS+=(SIMCTL_CHILD_$kv)
  done
fi

START_S=$(date +%s)
START_TIME=$(python3 -c 'import time;print(time.time())')

# ---------------------------------------------------------------- the launch
if [[ $APP_NAME == plain ]]; then
  typeset -a DEBUG_ARGS
  DEBUG_ARGS=()
  [[ ${WAIT_DEBUGGER:-0} == 1 ]] && DEBUG_ARGS=(--wait-for-debugger)
  env $ENVS xcrun simctl launch --console-pty $DEBUG_ARGS $UDID $BUNDLE > $OUT/app.log 2>&1 &
  LAUNCH_PID=$!
  wait_for_line $OUT/app.log DONE $PLAIN_WAIT_POLLS $LAUNCH_PID
  sleep 0.3
  kill $LAUNCH_PID 2>/dev/null   # it may have ended already
elif [[ $APP_NAME == canary ]]; then
  env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE > $OUT/app.log 2>&1 &
  LAUNCH_PID=$!

  BACKGROUNDED=0
  # wait for the canary's own budget (CANARY_BUDGET via XENV, default 45 s) plus the launch slack
  CANARY_BUDGET_S=$(print -r -- ${XENV:-} | grep -oE 'CANARY_BUDGET=[0-9]+' | tail -1 | cut -d= -f2)
  CANARY_BUDGET_S=${CANARY_BUDGET_S:-45}
  # not wait_for_line: also sends the canary to the background and back at its BG-REQUEST
  for i in {1..$(( (CANARY_BUDGET_S + CANARY_LAUNCH_SLACK_S) * 10 ))}; do   # 0.1 s polls
    sleep 0.1
    grep -q "RESULT" $OUT/app.log && break
    kill -0 $LAUNCH_PID 2>/dev/null || break
    if [[ $BACKGROUNDED == 0 ]] && grep -q "BG-REQUEST" $OUT/app.log; then
      BACKGROUNDED=1
      # the background trip: a launch that fails shows as the canary's foreground check failing
      xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1
      sleep 1.2
      xcrun simctl launch $UDID $BUNDLE >/dev/null 2>&1
    fi
  done

  # no RESULT line and the launch still running: the canary ran out of time (not a crash)
  STILL_RUNNING=
  if ! grep -q "RESULT" $OUT/app.log && kill -0 $LAUNCH_PID 2>/dev/null; then
    STILL_RUNNING=1
  fi

  sleep 0.3
  # whatever already ended: nothing to report
  kill $LAUNCH_PID 2>/dev/null
  xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null
else
  DATA=$(xcrun simctl get_app_container $UDID $BUNDLE data)
  mkdir -p $DATA/Documents
  rm -f $DATA/Documents/$APP_LOG_NAME
  [[ $LOG_MARK == M7 ]] && echo $FLUTTER_TEST > $DATA/Documents/m7test.txt

  env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE $ARGS > $OUT/launch.txt 2>&1 &
  LAUNCH_PID=$!

  typeset -A screenshot_taken

  # This loop doesn't use wait_for_line. It polls the app's own log file every second, copied out of its container.
  # It also takes screenshots on request, and logs "APP GONE" once the app is no longer running.
  # app.log may not exist yet, hence the 2>/dev/null below.
  for i in {1..$MAXW}; do
    sleep 1
    [[ -f $DATA/Documents/$APP_LOG_NAME ]] && cp $DATA/Documents/$APP_LOG_NAME $OUT/app.log
    for shot_name in $(grep -oE 'SCREENSHOT [a-z0-9-]+' $OUT/app.log 2>/dev/null | awk '{print $2}'); do
      if [[ -z ${screenshot_taken[$shot_name]:-} ]]; then
        # a screenshot is evidence only: one that fails changes no verdict
        xcrun simctl io $UDID screenshot $OUT/$shot_name.png >/dev/null 2>&1
        screenshot_taken[$shot_name]=1
      fi
    done
    grep -q "$END_PATTERN" $OUT/app.log 2>/dev/null && break
    if (( i > APP_START_GRACE_S )) && ! app_running; then
      echo "APP GONE after ${i}s" >> $OUT/app.log
      break
    fi
  done

  sleep 1
  # a screenshot is evidence only: one that fails changes no verdict; the launch may have ended
  xcrun simctl io $UDID screenshot $OUT/final.png >/dev/null 2>&1
  kill $LAUNCH_PID 2>/dev/null

  # each recorded file the app names: its media info (and audio, for movies)
  for f in $(grep -oE 'path=[^ ]+' $OUT/app.log 2>/dev/null | cut -d= -f2); do
    echo "$f :: $($ROOT/tools/bin/avinfo $f 2>&1)"
    [[ $f == *.mov || $f == *.mp4 ]] && $ROOT/tools/bin/avaudio $f
  done > $OUT/media.txt 2>&1
fi

ELAPSED=$(python3 -c "import time;print('%.1f'%(time.time()-$START_TIME))")
# either log may be missing: no pid then
PID=$(grep -ohE '\[simcam [0-9]+\]' $OUT/launch.txt $OUT/app.log 2>/dev/null | head -1 | grep -oE '[0-9]+')

# boot and launchd modes: dylib log is per pid in the boot log dir
# this app's own log (its pid), not the newest file: with lazy arming an app that never arms writes none
if [[ $INJECTION == boot || $INJECTION == launchd ]]; then
  # no file: the sim was not booted by boot.sh, so there is no engine log to copy
  BOOT_LOG_DIR=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt 2>/dev/null)
  if [[ -n $PID && -f $BOOT_LOG_DIR/dylib-$PID.log ]]; then
    cp $BOOT_LOG_DIR/dylib-$PID.log $OUT/dylib.log
  else
    echo "no engine log for this launch (pid ${PID:-unknown}): the app did not arm, or logged nothing" > $OUT/dylib.log
  fi
fi

# it may have exited already
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null

# ---------------------------------------------------------------- summary.txt
if [[ $APP_NAME == canary ]]; then
  RESULT=$(grep -o "RESULT.*" $OUT/app.log | head -1)
  # No verdict. If the app was still running when run.sh gave up, the machine was starved: INCONCLUSIVE.
  # If the app is gone without a verdict, it crashed: FAIL.
  LAST_CANARY_LINE=$(grep CANARY $OUT/app.log | tail -1 | cut -c1-160)
  if [[ -z $RESULT && -n ${STILL_RUNNING:-} ]]; then
    RESULT="RESULT core=INCONCLUSIVE ext=INCONCLUSIVE (no verdict;"
    RESULT+=" still running after $(( CANARY_BUDGET_S + CANARY_LAUNCH_SLACK_S )) s; last=$LAST_CANARY_LINE)"
  fi
  [[ -z $RESULT ]] && RESULT="RESULT core=FAIL ext=FAIL (no verdict; last=$LAST_CANARY_LINE)"

  # An engine MISMATCH that silently turns a fix off fails the run, whatever the canary said.
  # The one checked is `MISMATCH: photo stop`. It means a photo request's delegateStorage.delegateQueue lookup failed,
  # so a stop doesn't wait for photo callbacks, and that fix is off.
  # The line can be in the console (app.log) or in the per-pid engine log. The canary's own core verdict follows
  # "core was".
  MISMATCH=$(grep -h -m1 -oE 'MISMATCH: photo stop[^;]{0,120}' $OUT/app.log $OUT/dylib.log 2>/dev/null | head -1)
  if [[ -n $MISMATCH && $RESULT == RESULT\ core=* ]]; then
    CANARY_CORE=${RESULT#RESULT core=}      # e.g. "PASS ext=PASS …"
    CANARY_CORE=${CANARY_CORE%% *}          # its first word, the canary's core verdict: "PASS"
    RESULT="RESULT core=FAIL engine:[$MISMATCH; core was $CANARY_CORE] ${RESULT#RESULT core=* }"
  fi

  echo "$RESULT" | cut -c1-1500 > $OUT/summary.txt   # long enough for the core, timing and ext lists
elif [[ $APP_NAME == plain ]]; then
  PASS_N=$(grep -c "| PASS" $OUT/app.log)
  FAIL_N=$(grep -c "| FAIL" $OUT/app.log)
  END_N=$(grep -c "DONE" $OUT/app.log)
  FIRST_FAILS=$(grep '| FAIL' $OUT/app.log | head -3 | cut -c1-150 | tr '\n' '|')
  echo "pass=$PASS_N fail=$FAIL_N end=$END_N elapsed=${ELAPSED}s $FIRST_FAILS" > $OUT/summary.txt
else
  # app.log may be missing (the app never logged): every count is then empty or 0
  PASS_N=$(grep -c "PASS" $OUT/app.log 2>/dev/null)
  FAIL_N=$(grep -E "FAIL" $OUT/app.log 2>/dev/null | grep -vc isAvailableAsync)
  END_N=$(grep -c "$END_PATTERN" $OUT/app.log 2>/dev/null)

  KNOWN=
  if [[ $APP_NAME == vcfull* ]]; then
    # vcfull: pre-registered FAILs (README); fail= includes them; unexpected must be 0
    KNOWN_RX='known=[0-9]+ unexpected=[0-9]+ total=[0-9]+ expected_total=[0-9]+ na=[0-9]+'
    KNOWN="$(grep -oE "$KNOWN_RX" $OUT/app.log 2>/dev/null | tail -1) "
  fi

  FIRST_FAILS=$(grep -E 'FAIL' $OUT/app.log 2>/dev/null | grep -v isAvailableAsync | head -4 | cut -c1-150 |
    tr '\n' '|')
  echo "pass=$PASS_N fail=$FAIL_N ${KNOWN}end=$END_N elapsed=${ELAPSED}s $FIRST_FAILS" > $OUT/summary.txt
fi

# The canary's core verdict has three states. INCONCLUSIVE means only timing checks failed, or the budget ran out.
# It is retried once, and a second INCONCLUSIVE counts as FAIL, so a real hang still fails.
# SIMCAM_NO_RETRY=1 keeps the first verdict.
if [[ $APP_NAME == canary && ${RESULT:-} == *core=INCONCLUSIVE* ]] &&
  [[ -z ${SIMCAM_NO_RETRY:-} && -z ${_SIMCAM_RETRIED:-} ]]; then
  # the retry's output stays in its own result dir
  _SIMCAM_RETRIED=1 $0 $APP_NAME $RUNTIME $INJECTION ${TAG}-retry >/dev/null 2>&1
  # no summary: handled below
  RETRY_RESULT=$(cat $SIMCAM_RESULTS/$APP_NAME-$RUNTIME-$INJECTION-${TAG}-retry/summary.txt 2>/dev/null)
  if [[ -z $RETRY_RESULT ]]; then
    RETRY_RESULT="RESULT core=FAIL (the retry produced no verdict)"
  elif [[ $RETRY_RESULT == *core=INCONCLUSIVE* ]]; then
    RETRY_RESULT="${RETRY_RESULT/core=INCONCLUSIVE/core=FAIL} (inconclusive twice)"
  fi

  # the first attempt's verdict as first=… (no "core="), so a grep for core=INCONCLUSIVE / core=FAIL
  # counts final verdicts only
  FIRST_VERDICT=${RESULT#*core=}       # e.g. "INCONCLUSIVE ext=PASS …"
  FIRST_VERDICT=${FIRST_VERDICT%% *}   # its first word: "INCONCLUSIVE"
  FIRST_REST=$(print -r -- ${RESULT#*core=* } | cut -c1-200)
  echo "$RETRY_RESULT | retried once; first=$FIRST_VERDICT $FIRST_REST" | cut -c1-1800 > $OUT/summary.txt
fi

echo "$START_S" > $OUT/start.txt
echo "$APP_NAME/$RUNTIME/$INJECTION/$TAG :: $(cat $OUT/summary.txt)"
