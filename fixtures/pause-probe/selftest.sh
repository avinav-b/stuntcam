#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# selftest.sh <rt> [test ...] — checks of drive.sh itself, on our own sim with the probe installed.
#   timeout     drive.sh t-timeout waits for a marker the probe never logs: the case must fail with exit 1 and one
#               "FAIL t-timeout/<rt>: timed out …" line, and the probe must be gone afterwards
#   <case>:<sig>:<target>   start drive.sh <case> in its own process group, wait until it holds the app (c3-session: the
#               SIGSTOP; c1-photo: lldb stopped at the breakpoint), wait 2 s, then send <sig> (INT|TERM) to <target>:
#               group = the whole process group (what Ctrl-C in a terminal does), drive = drive.sh's pid only (a plain kill),
#               hold = hold.py's pid only. PASS when drive.sh exits within 15 s with 130/143 (hold: 1, the case failed) and
#               one FAIL line, no hold.py or debugserver for the app is left, and the app was resumed: its watchdog logged the
#               end of the pause after the signal (afterwards it is gone, since drive.sh ends the launch console on exit, or
#               running, untraced and not stopped).
#   c1-photo:INTTERM:hold   optional: a double signal before the hit. drive.sh c1-photo runs with PP_TRIG=- (no photo is
#               triggered, so hold.py sits in WaitForEvent with the breakpoint armed); 2 s after hold.py's "breakpoint" line,
#               SIGINT to hold.py and SIGTERM 50 ms later. PASS when drive.sh exits 1 within 15 s with one FAIL line, hold.py
#               logged "interrupted by signal 2" then "continued the app" or "detached" (no "cleanup error"), the breakpoint was
#               never hit, no hold.py or debugserver is left, and the app's watchdog logged "debugger detached" after the signal
#               (then the app is gone, or running, untraced and not stopped)
#   default: timeout and every <case>:<sig>:<target> of c3-session/c1-photo x INT/TERM x group/drive, plus c1-photo:TERM:hold
# Whatever happens, the test resumes and terminates the probe itself afterwards and kills only the processes it started.
# Results: $SIMCAM_RESULTS/<ST_RUN>-<n>-<case>-<rt>/ (drive.sh's dir; drive.txt = its stdout); ST_RUN defaults to e27st.
set -u; HERE=${0:A:h}; ROOT=${HERE:h:h}; source $ROOT/scripts/env.sh
RT=${1:?usage: $0 <rt> [test ...]}; shift; V=SIMCAM_UDID_$RT; U=${(P)V:-}; [[ -n $U ]] || { echo "$0: set $V" >&2; exit 2; }
typeset -a TESTS; TESTS=("$@"); (( ${#TESTS} )) || TESTS=(timeout c3-session:{INT,TERM}:{group,drive} c1-photo:{INT,TERM}:{group,drive} c1-photo:TERM:hold)
B=dev.simcam.e25.pauseprobe; ST_RUN=${ST_RUN:-e27st}
[[ -f $ROOT/build/bootlogdir-$RT.txt ]] || { echo "$0: no build/bootlogdir-$RT.txt: boot the sim with scripts/boot.sh $RT stub first" >&2; exit 2; }
BL=$(<$ROOT/build/bootlogdir-$RT.txt); fails=0; n=0
now() { python3 -c 'import time;print("%.3f"%(time.time()-978307200))'; }
ours() { [[ -n $1 && $(ps -o command= -p $1 2>/dev/null) == *"/Devices/$U/"*pauseprobe* ]]; }
ok() { echo "PASS $1 — $2"; }
bad() { echo "FAIL $1 — $2"; fails=$((fails + 1)); }
for t in $TESTS; do
  n=$((n + 1))
  if [[ $t == timeout ]]; then
    export PP_RUN=$ST_RUN-$n; out=$SIMCAM_RESULTS/$PP_RUN-t-timeout-$RT
    $HERE/drive.sh $RT t-timeout > $SIMCAM_RESULTS/$PP_RUN-drive.txt 2>&1; rc=$?
    mkdir -p $out; mv $SIMCAM_RESULTS/$PP_RUN-drive.txt $out/drive.txt
    pid=$(grep -oE 'READY pid=[0-9]+' $out/app.log 2>/dev/null | head -1 | cut -d= -f2)
    fl=$(grep -c "^FAIL t-timeout/$RT: timed out" $out/drive.txt); alive=no; ours ${pid:-} && alive=yes
    if (( rc == 1 && fl == 1 )) && [[ $alive == no ]]; then ok timeout "exit $rc; $(grep '^FAIL' $out/drive.txt)"
    else bad timeout "exit $rc, FAIL lines $fl, probe alive $alive: $(tail -2 $out/drive.txt | tr '\n' '|')"; fi
    ours ${pid:-} && xcrun simctl terminate $U $B 2>/dev/null
    continue
  fi
  c=${t%%:*}; r=${t#*:}; sig=${r%%:*}; tgt=${r#*:}
  export PP_RUN=$ST_RUN-$n; out=$SIMCAM_RESULTS/$PP_RUN-$c-$RT; rm -rf $out
  case $c in c3-session) mark='HOST\| .* STOP [0-9]+ epoch';; c1-photo) mark='HOLD\| .* rep 1: STOPPED at';; *) bad $t "unknown case"; continue;; esac
  trig=photo:3; [[ $sig == INTTERM ]] && { [[ $t == c1-photo:INTTERM:hold ]] || { bad $t "INTTERM is only c1-photo:INTTERM:hold"; continue; }
    trig=-; mark='HOLD\| .* breakpoint probe_photo_hit locations='; }   # no trigger: hold.py waits for a hit that never comes
  # its own session and process group, so "group" reaches drive.sh, its children (sleep, hold.py, simctl) and nothing else
  PP_TRIG=$trig python3 -c 'import os,sys; os.setsid(); os.execv(sys.argv[1], sys.argv[1:])' $HERE/drive.sh $RT $c > $SIMCAM_RESULTS/$PP_RUN-drive.txt 2>&1 &
  dp=$!
  for _ in {1..1200}; do grep -qE "$mark" $out/host.log 2>/dev/null && break; kill -0 $dp 2>/dev/null || break; sleep 0.1; done
  pid=$(grep -oE 'READY pid=[0-9]+' $out/app.log 2>/dev/null | head -1 | cut -d= -f2)
  if ! grep -qE "$mark" $out/host.log 2>/dev/null || ! ours ${pid:-}; then bad $t "never reached the hold (see $out)"; kill -KILL -- -$dp 2>/dev/null; continue; fi
  sleep 2; st0=$(ps -o stat= -p $pid | tr -d ' '); hp=$(pgrep -f "hold.py $pid " | head -1)
  t0=$(now); s0=$(date +%s)
  if [[ $sig == INTTERM ]]; then [[ -n $hp ]] && { kill -INT $hp; sleep 0.05; kill -TERM $hp; }   # Ctrl-C, then drive.sh's SIGTERM
  else case $tgt in group) kill -$sig -- -$dp;; drive) kill -$sig $dp;; hold) [[ -n $hp ]] && kill -$sig $hp;; esac; fi
  mid=; for _ in {1..150}; do   # INTTERM: once hold.py is gone, sample the app before drive.sh's exit ends it
    [[ $sig == INTTERM && -z $mid ]] && ! kill -0 ${hp:-0} 2>/dev/null && { mf=$(ps -o flags= -p $pid 2>/dev/null | tr -d ' ')
      mid="$(ps -o stat= -p $pid 2>/dev/null | tr -d ' ') $([[ -z $mf ]] && echo ? || { (( 16#$mf & 0x800 )) && echo traced || echo untraced; })"; }
    kill -0 $dp 2>/dev/null || break; sleep 0.1; done
  gone=yes; kill -0 $dp 2>/dev/null && gone=no; el=$(( $(date +%s) - s0 )); drc=running; [[ $gone == yes ]] && { wait $dp; drc=$?; }
  sleep 3
  st1=$(ps -o stat= -p $pid 2>/dev/null | tr -d ' '); [[ -z $st1 ]] && st1=gone
  fl=$(ps -o flags= -p $pid 2>/dev/null | tr -d ' '); tr1=untraced   # p_flag; P_TRACED = 0x800, what the engine's watchdog reads
  if [[ -z $fl ]]; then tr1=?; elif (( 16#$fl & 0x800 )); then tr1=traced; fi
  gl=$(awk -v t=$t0 '$1 > t && /watchdog: process was stopped for/' $BL/dylib-$pid.log 2>/dev/null | head -1)
  dl=$(awk -v t=$t0 '$1 > t && /watchdog: debugger detached/' $BL/dylib-$pid.log 2>/dev/null | head -1)
  hl=$(pgrep -f "hold.py $pid " | grep -c .); dsl=$(pgrep -f "debugserver.*[^0-9]$pid([^0-9]|\$)" | grep -c .)
  mv $SIMCAM_RESULTS/$PP_RUN-drive.txt $out/drive.txt 2>/dev/null
  { echo "test $t at $t0: before the signal stat=$st0 hold.py=${hp:-none}; drive.sh exited=$gone after ${el}s; after: stat=$st1 $tr1"
    echo "gap line: ${gl:-none}"; echo "detach line: ${dl:-none}"; echo "hold.py left: $hl, debugserver left: $dsl"; [[ -n $mid ]] && echo "app when hold.py had exited: $mid"; } > $out/selftest.txt
  ev="drive.sh exit $drc after ${el}s ($(grep -m1 '^FAIL' $out/drive.txt 2>/dev/null)); app stat $st0 -> $st1, $tr1; watchdog after: ${${gl#*\] }:-none}${dl:+, ${dl#*\] }}; hold.py left $hl, debugserver left $dsl"
  # the app is either running again (not stopped, not traced, and its watchdog logged the pause ending), or gone
  [[ $st1 == Z* ]] && st1=gone   # a zombie: exited, not yet reaped
  # resumed = the app's watchdog logged the end of the pause after the signal; then it may be running (not stopped, not
  # traced) or gone (drive.sh ends the launch console on exit, which ends the app)
  live=no; [[ -n $gl && ( $st1 == gone || ( $st1 != *T* && $tr1 == untraced ) ) ]] && live=yes
  if [[ $sig == INTTERM ]]; then   # before the hit the app isn't stopped, so no gap line: lldb must have detached, cleanly
    hc=$(awk '/HOLD\| .* interrupted by signal/{f=1} f && /^HOLD\|/' $out/host.log 2>/dev/null)
    hok=no; [[ $hc == *"interrupted by signal 2"* && ( $hc == *"continued the app"* || $hc == *"detached"* ) && $hc != *"cleanup error"* ]] && hok=yes
    grep -qE 'HOLD\| .* STOPPED at' $out/host.log 2>/dev/null && hok=hit
    live=no; [[ $hok == yes && -n $dl && $mid == [^T]*\ untraced && ( $st1 == gone || ( $st1 != *T* && $tr1 == untraced ) ) ]] && live=yes
    ev="$ev; app when hold.py had exited: ${mid:-not sampled}; hold.py after the signal: $(print -r -- $hc | sed 's/^HOLD| [0-9.]* //' | tr '\n' '|') ($hok)"
  fi
  want='(130|143)'; [[ $tgt == hold ]] && want=1   # hold.py killed alone: drive.sh fails the case (hold.py exit 143)
  fl=$(grep -c '^FAIL' $out/drive.txt 2>/dev/null)
  if [[ $gone == yes && $drc == ${~want} && $live == yes ]] && (( el <= 15 && hl == 0 && dsl == 0 && fl == 1 )); then ok $t "$ev"; else bad $t "$ev"; fi
  # clean up whatever is left: only what this test started
  kill -0 $dp 2>/dev/null && { kill -TERM -- -$dp 2>/dev/null; sleep 2; kill -KILL -- -$dp 2>/dev/null; }
  for h in $(pgrep -f "hold.py $pid "); do kill -TERM $h 2>/dev/null; done
  if ours $pid; then kill -CONT $pid; xcrun simctl terminate $U $B 2>/dev/null; fi
  sleep 1
done
(( fails )) && { echo "$fails test(s) failed"; exit 1; } || echo "all ${#TESTS} tests passed"
