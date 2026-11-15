#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# drive.sh <rt> <case>  — one U5 case on our own simulator. Results: $SIMCAM_RESULTS/e25-<case>-<rt>/
#   cases: c1-photo c1-kvo c1-vdo c1-kvorun (lldb 10 s holds x3) | c2-photo c2-photobg c2-rec (background x5) |
#          c3-session c3-photo c3-rec (kill -STOP 60 s x3) | c4 (4K60 30 s recording)
#          t-timeout (harness self-test: waits for a line the probe never logs; must FAIL)
# Every case ends with a check without any pause: one photo, a 2 s recording, stop + start; then status, exit, collect.
# Last line: "PASS <case>/<rt> :: …", exit 0; or "FAIL <case>/<rt>: <why>", exit 1, as soon as any wait times out, a hold
# or a STOP doesn't happen, or hold.py fails. SIGINT/SIGTERM end the case at once (exit 130/143), and on any exit the trap
# stops hold.py (its finally continues and detaches lldb), sends SIGCONT and ends the launch console: the app is never
# left stopped. Test: selftest.sh.
set -u; HERE=${0:A:h}; ROOT=${HERE:h:h}; source $ROOT/scripts/env.sh; source $HERE/lib.sh; RT=$1; CASE=$2
PY=/Applications/Xcode.app/Contents/Developer/usr/bin/python3; LLDBPY=$(xcrun lldb -P)
STOPS=${STOPS:-60}
setup_running() { pp_cmd setup ${1:-}; pp_waitn 'NOTE DidStartRunning' 1 15; sleep 3; }
postcheck() {   # ops with no pause, after the case: each must complete
  local p=$(( $(pp_count 'PHOTO-REQ') + 1 )) m=$(( $(pp_count 'REC-START') + 1 ))
  pp_log "postcheck start"
  pp_cmd photo 1; pp_waitn "PHOTO-DONE $p " 1 15
  pp_cmd rec start; pp_waitn "MOVIE-STARTED mov-$m" 1 10; sleep 2; pp_cmd rec stop; pp_waitn "MOVIE-DONE mov-$m" 1 20
  pp_cmd stop; pp_waitn 'RET stop' 1 10; pp_cmd start; pp_waitn 'RET start' 2 10; sleep 2
  pp_log "postcheck end"
}
hold() { pp_ours || pp_fail "refusing lldb: pid $PID is not our probe"
  pp_run env PYTHONPATH=$LLDBPY $PY $HERE/hold.py $PID $1 3 10 $CMD $2 $3 $4 >> $OUT/host.log 2>&1 || pp_fail "hold.py $1 failed (exit $?; see host.log)"; }
# on any exit, never leave our app stopped or a launch console behind. hold.py first: SIGTERM runs its
# finally (continue, detach), and it must finish before the app is resumed and the console goes (10 s at most).
# SIGCONT, then 0.5 s for the app's watchdog to log the end of the pause, then end the console (that ends the app)
on_exit() { pp_stopchild; [[ -n ${PID:-} ]] && kill -0 $PID 2>/dev/null && { kill -CONT $PID; sleep 0.5; }; [[ -n ${LP:-} ]] && kill $LP 2>/dev/null; return 0; }
exec 3>&1   # the traps print here: a trap can run inside a function whose output goes to host.log (hold)
trap on_exit EXIT
# zsh resumes the script after a trap that doesn't exit, so these exit (the EXIT trap then cleans up); pp_stopchild first,
# or zsh holds the exit until a pp_run child (sleep 60, hold.py) ends by itself
trap 'pp_log "SIGINT: ending the case"; print -u3 -r -- "FAIL $CASE/$RT: interrupted (SIGINT)"; pp_stopchild; exit 130' INT
trap 'pp_log "SIGTERM: ending the case"; print -u3 -r -- "FAIL $CASE/$RT: interrupted (SIGTERM)"; pp_stopchild; exit 143' TERM
pp_launch $RT $CASE
case $CASE in
  c1-photo)  setup_running; hold probe_photo_hit ${PP_TRIG:-photo:3} - 6;;   # PP_TRIG=-: no trigger (selftest c1-photo:INTTERM:hold)
  c1-kvo)    setup_running; hold probe_kvo_hit focus - 4;;
  c1-vdo)    setup_running; hold probe_vdo_hit - - 4;;
  c1-kvorun) setup_running; hold probe_kvo_running_hit stop start 5;;
  c2-photo|c2-photobg)  setup_running; ARM=armresign; [[ $CASE == c2-photobg ]] && ARM=armbg
    for i in {1..5}; do pp_log "rep $i"; pp_cmd $ARM 5; sleep 0.3; pp_bg; pp_run sleep 10; pp_fg
      pp_waitn 'NOTE InterruptionEnded' $i 15; pp_waitn 'PHOTO-DONE' $((5 * i)) 15; sleep 3; done;;
  c2-rec)    setup_running
    for i in {1..5}; do pp_log "rep $i"; pp_cmd rec start; pp_waitn "MOVIE-STARTED mov-$i" 1 10; sleep 2; pp_bg; pp_run sleep 10; pp_fg
      pp_waitn 'NOTE InterruptionEnded' $i 15; pp_waitn "MOVIE-DONE mov-$i" 1 20; sleep 3; done;;
  c3-session) setup_running
    for i in {1..3}; do pp_log "rep $i"; pp_stop; pp_run sleep $STOPS; pp_cont; sleep 6; done;;
  c3-photo)  setup_running
    for i in {1..3}; do pp_log "rep $i"; pp_ours || pp_fail "pid $PID is not our probe"
      pp_cmd photo 10; pp_run python3 $HERE/onmatch.py $OUT/app.log "PHOTO-REQ $((10 * (i - 1) + 3)) " 10 $PID >> $OUT/host.log \
        && pp_log "STOP (onmatch) epoch $(date +%s)" || pp_fail "onmatch timed out: no STOP in rep $i"
      pp_run sleep $STOPS; pp_cont; pp_waitn 'PHOTO-DONE' $((10 * i)) 30; sleep 3; done;;
  c3-rec)    setup_running
    for i in {1..3}; do pp_log "rep $i"; pp_cmd rec start; pp_waitn "MOVIE-STARTED mov-$i" 1 10; sleep 2
      pp_stop; pp_run sleep $STOPS; pp_cont; sleep 3; pp_cmd rec stop; pp_waitn "MOVIE-DONE mov-$i" 1 60; sleep 2; done;;
  c4)        setup_running 4k60
    pp_cmd rec start; pp_waitn 'MOVIE-STARTED mov-1' 1 10; pp_run sleep 30; pp_cmd rec stop; pp_waitn 'MOVIE-DONE mov-1' 1 180; sleep 2;;
  t-timeout) setup_running; pp_waitn 'NEVER-LOGGED-BY-THE-PROBE' 1 5;;
  *)         pp_fail "unknown case $CASE";;
esac
postcheck
pp_finish
print -r -- "PASS $CASE/$RT :: $(grep -E 'MOVIE-DONE|PHOTO-DONE' $OUT/app.log | grep -c 'err=nil\|readable=true') ok-lines, hang=$(grep -c ' hang ' $OUT/events.txt) unknown-hang=$(grep -c unknown-hang $OUT/events.txt) latched=$(grep -c latched $OUT/dylib.log) gaps=$(grep -c 'process was stopped' $OUT/dylib.log) $(grep -E 'TIMEOUT|FAILED|NOT hit|refusing' $OUT/host.log | head -3 | tr '\n' '|')"
