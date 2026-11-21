#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# webcam-test.sh <group> — the webcam source end to end: helper (helper/dev.sh) + engine + fixtures/plain in
# M5APP_MODE=webcam (scripts/webcam-app.sh), on the sims $SIMCAM_UDID_263 and $SIMCAM_UDID_27 (armed: boot.sh <rt> stub).
# Each group starts the helpers it needs and stops them again. Results: $SIMCAM_RESULTS/webcam-<rt>-<tag>/.
#   synth      synthetic helper; 10 s of video data, a photo and a 5 s movie on each runtime, with the pump locked to the
#              ring and then free-running (SIMCAM_WEBCAM_PHASE=0), for the latency comparison
#   rate60     synthetic 30 fps helper, 1080p60 format: every other tick a new frame
#   stopstart  synthetic; the helper is stopped (SIGTERM) 6 s into a 20 s run and started again 5 s later; then the same
#              with kill -9 (a crash: the ring stays `running`, frames go stale)
#   two        one synthetic helper, the app on both runtimes at the same time
#   nohelper   no helper at all: the scene with the offline band; a screenshot of it (band.png)
#   switch     scene -> webcam -> scene through the control file mid-run: the pump goes back to its nominal schedule
#   idle       the helper closes the camera 10 s after the last active reader and quits after --quit-after (20 s here)
#   real [ID]  the real camera (default selection, or uniqueID ID), stamped; the helper must already be authorized
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh
G=${1:?group}; shift
APP=$ROOT/scripts/webcam-app.sh; H=$ROOT/helper/dev.sh
res() { print -r -- "$SIMCAM_RESULTS/webcam-$1-$2"; }
cam_of() { python3 -c "
import sys
h=0xcbf29ce484222325
for b in sys.argv[1].encode()+b'\0'+sys.argv[2].encode(): h=((h^b)*0x100000001b3)&0xffffffffffffffff
print('%08x'%(h&0xffffffff))" "$SIMCAM_HOME" "$1"; }
pidfile_pid() { sed -n 's/^pid=//p' $SIMCAM_HOME/webcam/$(cam_of $1).pid 2>/dev/null; }
helperlog() { print -r -- "$SIMCAM_HOME/logs/webcam-$(cam_of $1).log"; }
# wait_running <result dir>: up to 15 s (150 polls of 0.1 s) for the app's wc.running line
wait_running() { local i; for i in {1..150}; do sleep 0.1; grep -q wc.running $1/app.log 2>/dev/null && break; done; }
case $G in
  synth)
    $H start --synthetic >/dev/null || exit 1
    for rt in 263 27; do $APP $rt syn WC_SECS=10 WC_MOVIE=5; $APP $rt syn-free WC_SECS=10 WC_MOVIE=0 WC_PHOTO=0 SIMCAM_WEBCAM_PHASE=0; done
    $H stop;;
  rate60)
    $H start --synthetic >/dev/null || exit 1
    for rt in 263 27; do $APP $rt syn60 WC_SECS=10 WC_FPS=60 WC_MOVIE=0 WC_PHOTO=0; done
    $H stop;;
  stopstart)
    for how in term kill; do
      $H start --synthetic >/dev/null || exit 1
      $APP 263 ss-$how WC_SECS=20 WC_MOVIE=0 WC_PHOTO=0 > $SIMCAM_RESULTS/webcam-ss-$how.txt 2>&1 &
      AP=$!; O=$(res 263 ss-$how)
      wait_running $O
      sleep 7; P=$(pidfile_pid synthetic); echo "$(date +%T) stopping helper $P ($how)"
      if [[ $how == term ]]; then $H stop; else kill -9 $P; fi
      sleep 5; echo "$(date +%T) starting the helper again"; $H start --synthetic >/dev/null
      wait $AP; cat $SIMCAM_RESULTS/webcam-ss-$how.txt
      grep -E "WCSEC" $O/app.log | cut -c8- | tr '\n' ' ' | sed 's/WCSEC //g'; echo
      grep -hE "webcam: (live|offline|ring /)" $O/dylib.log | cut -c20-200
      $H stop
    done;;
  two)
    $H start --synthetic >/dev/null || exit 1
    $APP 263 two WC_SECS=10 WC_MOVIE=5 > $SIMCAM_RESULTS/webcam-two-263.txt 2>&1 & A1=$!
    $APP 27 two WC_SECS=10 WC_MOVIE=5 > $SIMCAM_RESULTS/webcam-two-27.txt 2>&1 & A2=$!
    wait $A1 $A2; cat $SIMCAM_RESULTS/webcam-two-263.txt $SIMCAM_RESULTS/webcam-two-27.txt
    grep -E "readers:" "$(helperlog synthetic)" | tail -3 | cut -c1-300
    $H stop;;
  nohelper)
    $H stop >/dev/null
    $APP 263 nohelper WC_SECS=6 WC_MOVIE=0 > $SIMCAM_RESULTS/webcam-nohelper.txt 2>&1 & AP=$!
    O=$(res 263 nohelper); wait_running $O
    sleep 3; xcrun simctl io $SIMCAM_UDID_263 screenshot $O/band.png >/dev/null 2>&1
    wait $AP; cat $SIMCAM_RESULTS/webcam-nohelper.txt;;
  switch)
    $H start --synthetic >/dev/null || exit 1
    SRC=scene $APP 263 switch WC_SECS=15 WC_MOVIE=0 WC_PHOTO=0 > $SIMCAM_RESULTS/webcam-switch.txt 2>&1 & AP=$!
    O=$(res 263 switch); wait_running $O
    sleep 3; echo "source=webcam:synthetic" > $O/control.txt; sleep 6; echo "source=scene" > $O/control.txt
    wait $AP; cat $SIMCAM_RESULTS/webcam-switch.txt
    grep -E "WCSEC" $O/app.log | cut -c8- | tr '\n' ' ' | sed 's/WCSEC //g'; echo
    grep -hE "source (->|applied)|webcam: (live|pump|source left|reader)" $O/dylib.log | cut -c20-200
    ls $SIMCAM_HOME/webcam/readers/
    $H stop;;
  idle)
    $H start --synthetic --quit-after 20 >/dev/null || exit 1
    $APP 263 idle WC_SECS=3 WC_MOVIE=0 WC_PHOTO=0 >/dev/null 2>&1
    T=$(date +%s); P=$(pidfile_pid synthetic)
    for i in {1..60}; do sleep 1; kill -0 $P 2>/dev/null || break; done
    echo "helper $P gone $(( $(date +%s) - T )) s after the app finished"
    grep -E "readers:|opening|closing|quitting|retired|exit" "$(helperlog synthetic)" | tail -7 | cut -c1-200;;
  real)
    ID=${1:-}; typeset -a CA; CA=(); [[ -n $ID ]] && CA=(--camera $ID)
    $H start --stamp $CA >/dev/null || exit 1
    for rt in 263 27; do SRC=webcam${ID:+:$ID} $APP $rt real WC_SECS=10 WC_MOVIE=5
      SRC=webcam${ID:+:$ID} $APP $rt real-free WC_SECS=10 WC_MOVIE=0 WC_PHOTO=0 SIMCAM_WEBCAM_PHASE=0; done
    $H stop;;
  *) print_usage $0; exit 2;;
esac
