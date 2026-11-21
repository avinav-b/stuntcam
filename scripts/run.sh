#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# run.sh <app> <rt: 263|27|175> <inj: boot|launchd|launch|none> <tag>
#   app: canary | vc | vc-audio | vcfull | vcfull-audio | expo | expo-audio | flutter (all tests) | flutter-<test>
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
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh; APPN=$1; RT=$2; INJ=$3; TAG=${4:-x}
need_udid "$0" "$RT" UDID long
OUT=$SIMCAM_RESULTS/$APPN-$RT-$INJ-$TAG; rm -rf $OUT; mkdir -p $OUT
DY=${DYLIB:-$(cat $ROOT/build/current_dylib.txt)}; PL=${PLIST:-$SIMCAM_DEV/profiles/cfg-m10.plist}
typeset -a ENVS ARGS; ENVS=(); ARGS=()
EXPO_PORT=${EXPO_PORT:-8099}
LOGN=m1b.log; MARK=M1B; ENDRE="END "; MAXW=${MAXW:-}
case $APPN in
  canary) B=dev.simcam.spike.m10canary;;
  plain) B=dev.simcam.spike.m10plain;;
  vc|expo) B=dev.spike.m1bcam; ARGS=(-CAMTEST $APPN -RCT_jsLocation localhost:$EXPO_PORT);;
  vc-audio|expo-audio) B=dev.spike.m1bcam; ARGS=(-CAMTEST ${APPN%-audio} -AUDIO 1 -RCT_jsLocation localhost:$EXPO_PORT);;
  vcfull) B=dev.spike.m1bcam; ARGS=(-CAMTEST vcfull -RCT_jsLocation localhost:$EXPO_PORT); : ${MAXW:=300};;   # exhaustive VisionCamera checks (formats, photos, recordings); 4K encodes are slow
  vcfull-audio) B=dev.spike.m1bcam; ARGS=(-CAMTEST vcfull -AUDIO 1 -RCT_jsLocation localhost:$EXPO_PORT); : ${MAXW:=300};;
  flutter*) B=dev.spike.m7cam; LOGN=m7.log; MARK=M7; ENDRE="END"; FT=${APPN#flutter}; FT=${FT#-}; FT=${FT:-all};;
esac
MAXW=${MAXW:-150}
xcrun simctl terminate $UDID $B 2>/dev/null
xcrun simctl privacy $UDID grant camera $B 2>/dev/null; [[ ${NO_MIC_GRANT:-0} == 1 ]] || xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
if [[ $INJ == launch ]]; then
  inject_env "$DY" "$PL" "$OUT/dylib.log"
  ENVS=($INJECT_ENV SIMCTL_CHILD_SIMCAM_FIXTURE_IMAGE=$SIMCAM_DEV/media/qr.png SIMCTL_CHILD_M5_IMAGE=$SIMCAM_DEV/media/qr.png)
fi
# XENV: K=V words split on whitespace, or one K=V per line when it holds a newline (values with spaces, e.g. paths under
# "Application Support")
if [[ ${XENV:-} == *$'\n'* ]]; then for kv in "${(@f)XENV}"; do [[ -n $kv ]] && ENVS+=("SIMCTL_CHILD_$kv"); done
else for kv in ${=XENV:-}; do ENVS+=(SIMCTL_CHILD_$kv); done; fi
START=$(date +%s); T0=$(python3 -c 'import time;print(time.time())')
if [[ $APPN == plain ]]; then
  typeset -a WD; WD=(); [[ ${WAIT_DEBUGGER:-0} == 1 ]] && WD=(--wait-for-debugger)
  env $ENVS xcrun simctl launch --console-pty $WD $UDID $B > $OUT/app.log 2>&1 &
  LP=$!
  wait_for_line $OUT/app.log DONE 300 $LP   # up to 30 s
  sleep 0.3; kill $LP 2>/dev/null
elif [[ $APPN == canary ]]; then
  env $ENVS xcrun simctl launch --console-pty $UDID $B > $OUT/app.log 2>&1 &
  LP=$!; BGDONE=0
  # wait for the canary's own budget (CANARY_BUDGET via XENV, default 45 s) plus 15 s for launch
  CB=$(print -r -- ${XENV:-} | grep -oE 'CANARY_BUDGET=[0-9]+' | tail -1 | cut -d= -f2); CB=${CB:-45}
  # not wait_for_line: also sends the canary to the background and back at its BG-REQUEST
  for i in {1..$(( (CB + 15) * 10 ))}; do sleep 0.1
    grep -q "RESULT" $OUT/app.log && break
    kill -0 $LP 2>/dev/null || break
    if [[ $BGDONE == 0 ]] && grep -q "BG-REQUEST" $OUT/app.log; then BGDONE=1
      xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1; sleep 1.2
      xcrun simctl launch $UDID $B >/dev/null 2>&1; fi
  done
  STILL_RUNNING=; grep -q "RESULT" $OUT/app.log || ! kill -0 $LP 2>/dev/null || STILL_RUNNING=1
  sleep 0.3; kill $LP 2>/dev/null; xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null
else
  DATA=$(xcrun simctl get_app_container $UDID $B data); mkdir -p $DATA/Documents; rm -f $DATA/Documents/$LOGN
  [[ $MARK == M7 ]] && echo $FT > $DATA/Documents/m7test.txt
  env $ENVS xcrun simctl launch --console-pty $UDID $B $ARGS > $OUT/launch.txt 2>&1 &
  LP=$!
  typeset -A shot
  # not wait_for_line: 1 s polls of the app's own log file (copied out of its container), screenshots on request, and
  # "APP GONE" once the app is no longer running
  for i in {1..$MAXW}; do sleep 1
    [[ -f $DATA/Documents/$LOGN ]] && cp $DATA/Documents/$LOGN $OUT/app.log
    for s in $(grep -oE 'SCREENSHOT [a-z0-9-]+' $OUT/app.log 2>/dev/null | awk '{print $2}'); do
      [[ -z ${shot[$s]:-} ]] && { xcrun simctl io $UDID screenshot $OUT/$s.png >/dev/null 2>&1; shot[$s]=1; }; done
    grep -q "$ENDRE" $OUT/app.log 2>/dev/null && break
    if (( i > 6 )) && ! xcrun simctl spawn $UDID launchctl list 2>/dev/null | grep -q "UIKitApplication:$B"
    then echo "APP GONE after ${i}s" >> $OUT/app.log; break; fi
  done
  sleep 1; xcrun simctl io $UDID screenshot $OUT/final.png >/dev/null 2>&1
  kill $LP 2>/dev/null
  for f in $(grep -oE 'path=[^ ]+' $OUT/app.log 2>/dev/null | cut -d= -f2)
  do echo "$f :: $($ROOT/tools/bin/avinfo $f 2>&1)"; [[ $f == *.mov || $f == *.mp4 ]] && $ROOT/tools/bin/avaudio $f
  done > $OUT/media.txt 2>&1
fi
ELAPSED=$(python3 -c "import time;print('%.1f'%(time.time()-$T0))")
PID=$(grep -ohE '\[simcam [0-9]+\]' $OUT/launch.txt $OUT/app.log 2>/dev/null | head -1 | grep -oE '[0-9]+')
# boot and launchd modes: dylib log is per pid in the boot log dir
# this app's own log (its pid), not the newest file: with lazy arming an app that never arms writes none
if [[ $INJ == boot || $INJ == launchd ]]; then BL=$(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)
  if [[ -n $PID && -f $BL/dylib-$PID.log ]]; then cp $BL/dylib-$PID.log $OUT/dylib.log
  else echo "no engine log for this launch (pid ${PID:-unknown}): the app did not arm, or logged nothing" > $OUT/dylib.log; fi
fi
xcrun simctl terminate $UDID $B 2>/dev/null
if [[ $APPN == canary ]]; then
  R=$(grep -o "RESULT.*" $OUT/app.log | head -1)
  # no verdict: still running when run.sh gave up is INCONCLUSIVE (a starved machine); gone without one (a crash) is FAIL
  [[ -z $R && -n ${STILL_RUNNING:-} ]] &&
    R="RESULT core=INCONCLUSIVE ext=INCONCLUSIVE (no verdict; still running after $(( CB + 15 )) s; last=$(grep CANARY $OUT/app.log | tail -1 | cut -c1-160))"
  [[ -z $R ]] && R="RESULT core=FAIL ext=FAIL (no verdict; last=$(grep CANARY $OUT/app.log | tail -1 | cut -c1-160))"
  # an engine MISMATCH that silently turns a fix off fails the run, whatever the canary said:
  # `MISMATCH: photo stop` (a photo request's delegateStorage.delegateQueue lookup failed, so a stop doesn't wait for photo
  # callbacks: L66 off). In the console (app.log) or the per-pid engine log. The canary's own core verdict follows "core was".
  MM=$(grep -h -m1 -oE 'MISMATCH: photo stop[^;]{0,120}' $OUT/app.log $OUT/dylib.log 2>/dev/null | head -1)
  [[ -n $MM && $R == RESULT\ core=* ]] && R="RESULT core=FAIL engine:[$MM; core was ${${R#RESULT core=}%% *}] ${R#RESULT core=* }"
  echo "$R" | cut -c1-1500 > $OUT/summary.txt   # long enough for the core, timing and ext lists
elif [[ $APPN == plain ]]; then
  P=$(grep -c "| PASS" $OUT/app.log); F=$(grep -c "| FAIL" $OUT/app.log); E=$(grep -c "DONE" $OUT/app.log)
  echo "pass=$P fail=$F end=$E elapsed=${ELAPSED}s $(grep '| FAIL' $OUT/app.log | head -3 | cut -c1-150 | tr '\n' '|')" > $OUT/summary.txt
else
  P=$(grep -c "PASS" $OUT/app.log 2>/dev/null); F=$(grep -E "FAIL" $OUT/app.log 2>/dev/null | grep -vc isAvailableAsync)
  E=$(grep -c "$ENDRE" $OUT/app.log 2>/dev/null)
  K=; [[ $APPN == vcfull* ]] &&
    K="$(grep -oE 'known=[0-9]+ unexpected=[0-9]+ total=[0-9]+ expected_total=[0-9]+ na=[0-9]+' $OUT/app.log 2>/dev/null | tail -1) "   # vcfull: pre-registered FAILs (README); fail= includes them; unexpected must be 0
  echo "pass=$P fail=$F ${K}end=$E elapsed=${ELAPSED}s $(grep -E 'FAIL' $OUT/app.log 2>/dev/null | grep -v isAvailableAsync | head -4 | cut -c1-150 | tr '\n' '|')" > $OUT/summary.txt
fi
# canary tri-state (design §5.2): an INCONCLUSIVE core (only timing checks failed, or the budget ran out) is retried once;
# a second INCONCLUSIVE counts as FAIL, so a real hang still fails. SIMCAM_NO_RETRY=1 keeps the first verdict.
if [[ $APPN == canary && ${R:-} == *core=INCONCLUSIVE* && -z ${SIMCAM_NO_RETRY:-} && -z ${_SIMCAM_RETRIED:-} ]]; then
  _SIMCAM_RETRIED=1 $0 $APPN $RT $INJ ${TAG}-retry >/dev/null 2>&1
  R2=$(cat $SIMCAM_RESULTS/$APPN-$RT-$INJ-${TAG}-retry/summary.txt 2>/dev/null)
  if [[ -z $R2 ]]; then R2="RESULT core=FAIL (the retry produced no verdict)"
  elif [[ $R2 == *core=INCONCLUSIVE* ]]; then R2="${R2/core=INCONCLUSIVE/core=FAIL} (inconclusive twice)"; fi
  # the first attempt's verdict as first=… (no "core="), so a grep for core=INCONCLUSIVE / core=FAIL counts final verdicts only
  echo "$R2 | retried once; first=${${R#*core=}%% *} $(print -r -- ${R#*core=* } | cut -c1-200)" | cut -c1-1800 > $OUT/summary.txt
fi
echo "$START" > $OUT/start.txt
echo "$APPN/$RT/$INJ/$TAG :: $(cat $OUT/summary.txt)"
