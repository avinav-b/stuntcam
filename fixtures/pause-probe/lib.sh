# SPDX-License-Identifier: Apache-2.0
# lib.sh — sourced by drive.sh. Launch, command and collect one pause-probe run on our own simulator.
#   pp_launch <rt> <name>   (re)launch the probe; sets UDID OUT DATA CMD PID LP; waits for READY; checks the pid is ours
#   pp_cmd <cmd> [arg]      append a command line; pp_waitn <regex> <n> <timeout s> waits for >= n matching app.log lines.
#                           A wait that times out fails the case (pp_fail).
#   pp_fail <why>           print "FAIL <case>/<rt>: <why>", collect what there is (pp_finish), exit 1
#   pp_run <cmd ...>        run a command in the background and wait for it, so a SIGINT/SIGTERM trap runs at once (zsh
#                           defers traps until a foreground child exits); PP_CHILD is its pid meanwhile; pp_stopchild ends it
#   pp_finish               status + exit, then copy the engine log, this activation's events and `stuntcam sentinel ls`
#   PP_RUN (default e25): the result-dir prefix, $SIMCAM_RESULTS/<PP_RUN>-<case>-<rt>/, so a new run keeps an old run's results
B=dev.simcam.e25.pauseprobe
ROOT=${ROOT:-${0:A:h:h:h}}
pp_now() { python3 -c 'import time;print("%.3f"%(time.time()-978307200))'; }
pp_log() { print -r -- "HOST| $(pp_now) $*" >> $OUT/host.log; }
pp_launch() {
  local rt=$1 name=$2 v=SIMCAM_UDID_$1; UDID=${(P)v}; RT=$rt
  OUT=$SIMCAM_RESULTS/${PP_RUN:-e25}-$name-$rt; rm -rf $OUT; mkdir -p $OUT
  xcrun simctl terminate $UDID $B 2>/dev/null
  xcrun simctl privacy $UDID grant camera $B; xcrun simctl privacy $UDID grant microphone $B
  DATA=$(xcrun simctl get_app_container $UDID $B data); mkdir -p $DATA/Documents; CMD=$DATA/Documents/cmd.txt
  rm -f $CMD; local k; for k in {1..20}; do rm -f $DATA/Documents/mov-$k.mov; done
  # the console gets its own process group: Ctrl-C on drive.sh must not reach it (simctl would take the app down while
  # lldb still holds it, which wedged the sim's app services once in e27); the EXIT trap ends it after the app is resumed
  perl -e 'setpgrp(0, 0); exec @ARGV' xcrun simctl launch --console-pty $UDID $B > $OUT/app.log 2>&1 &
  LP=$!
  PP_NAME=$name; pp_waitn 'READY pid=' 1 30
  PID=$(grep -oE 'READY pid=[0-9]+' $OUT/app.log | head -1 | cut -d= -f2)
  local c=$(ps -o command= -p $PID)
  [[ $c == *"/Devices/$UDID/"*pauseprobe* ]] || { local p=$PID; PID=; pp_fail "pid $p is not our probe: $c"; }
  pp_log "launched $name on $rt ($UDID) pid $PID"
}
pp_cmd() { local n=0; [[ -f $CMD ]] && n=$(wc -l < $CMD | tr -d ' '); print -r -- "$((n + 1)) $1 ${2:-}" >> $CMD; pp_log "cmd $1 ${2:-}"; }
pp_count() { local c; c=$(grep -cE "$1" $OUT/app.log 2>/dev/null); print -r -- ${c:-0}; }
pp_waitn() { local re=$1 n=$2 t=$3 i; for i in {1..$((t * 20))}; do (( $(pp_count "$re") >= n )) && return 0; sleep 0.05; done
  pp_log "TIMEOUT waiting for $n x '$re'"; pp_fail "timed out after $t s waiting for $n x '$re' in app.log (saw $(pp_count "$re"))"; }
pp_fail() {
  [[ -n ${PP_FAILING:-} ]] && return 1; PP_FAILING=1   # once: pp_finish below must not fail the case again
  pp_log "FAIL: $*"; print -r -- "FAIL ${PP_NAME:-?}/${RT:-?}: $*"
  if [[ -n ${PID:-} ]]; then kill -CONT $PID 2>/dev/null; pp_finish; else kill ${LP:-} 2>/dev/null; xcrun simctl terminate ${UDID:-none} $B 2>/dev/null; fi
  exit 1
}
pp_run() { "$@" & PP_CHILD=$!; wait $PP_CHILD; local rc=$?; PP_CHILD=; return $rc; }
# end pp_run's child (SIGTERM; hold.py's finally continues the app and detaches), waiting up to 10 s. A trap must do this
# before `exit`: zsh holds an exit from a trap until the function's `wait` returns, i.e. until the child is gone.
pp_stopchild() {
  local c=${PP_CHILD:-} i; [[ -n $c ]] && kill -0 $c 2>/dev/null || return 0
  kill -TERM $c 2>/dev/null; for i in {1..100}; do kill -0 $c 2>/dev/null || break; sleep 0.1; done; kill -KILL $c 2>/dev/null; return 0
}
pp_ours() { [[ -n $PID && $(ps -o command= -p $PID) == *"/Devices/$UDID/"*pauseprobe* ]]; }
pp_stop() { pp_ours || pp_fail "refusing STOP: pid $PID is not our probe"; kill -STOP $PID; pp_log "STOP $PID epoch $(date +%s)"; }
pp_cont() { kill -CONT $PID; pp_log "CONT $PID epoch $(date +%s)"; }
pp_bg() { xcrun simctl launch $UDID com.apple.Preferences >/dev/null 2>&1; pp_log "backgrounded (Preferences launched)"; }
pp_fg() { xcrun simctl launch $UDID $B >/dev/null 2>&1; pp_log "foregrounded (probe launched again)"; }
pp_finish() {
  pp_cmd status; sleep 1; pp_cmd exit; sleep 1.5
  kill $LP 2>/dev/null; xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl terminate $UDID com.apple.Preferences 2>/dev/null
  local BL=$(cat $ROOT/build/bootlogdir-$RT.txt)
  cp $BL/dylib-$PID.log $OUT/dylib.log 2>/dev/null || echo "no engine log for pid $PID" > $OUT/dylib.log
  local act=$(grep -oE 'sentinel: activation [0-9]+' $OUT/dylib.log | head -1 | awk '{print $3}')
  local EV="$SIMCAM_HOME/sims/$UDID/state/$B/events.log"
  awk -v a=$act '$2 == a' "$EV" > $OUT/events.txt
  $ROOT/cli/bin/stuntcam sentinel ls --udid $UDID --bundle $B > $OUT/sentinel.txt 2>&1
  pp_log "finished; activation $act"
}
