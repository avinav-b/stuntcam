# SPDX-License-Identifier: Apache-2.0
# activation/timing.sh — bounded activation: the first call's wait, timeouts, pauses, a late prepare, a debugger attached.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
c_act-slow-ok() {
  clear_sentinel $BP; run plain so SIMCAM_TEST_SLOW_ACTIVATE_MS=300; local d=$(res plain so) f
  f=$(fc $d)
  # first-call ms: at least prepare's 300 ms sleep, at most 150 ms more for the rest of the activation
  local min_ms=300 max_ms=450
  if in_range "$f" $min_ms $max_ms && [[ $(pass_n $d) == 14 && $(cnt 'LAZY armed by' $d/dylib.log) == 1 ]]
  then ok "firstcall ${f} ms, 14/14, one armed-by line"
  else bad "firstcall ${f:-?} ms, pass $(pass_n $d), armed-by lines $(cnt 'LAZY armed by' $d/dylib.log)"; fi
  clear_sentinel $BP
}
c_act-slow-timeout() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d f lc h cr pid; n0=$(nl $e)
  run plain st SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d=$(res plain st)
  f=$(fc $d); lc=$(grep -oE 'latercall\.ms=[0-9.]+' $d/app.log | cut -d= -f2)
  pid=$(fault_pid $d/app.log); crash_reports m5app 0 $pid; cr=$CR; [[ -n $pid ]] || cr=no-pid
  h=$(since $e $n0 | grep -c 'hang activate .* phase=preflight')
  if in_range "$f" 900 1100 && (( ${lc%.*} < 5 )) && [[ $(cnt DONE $d/app.log) == 1 &&
    $(cnt 'finished after .* staying stock' $d/dylib.log) == 1 && $(cnt 'swap:' $d/dylib.log) == 0 && $h == 1 &&
    $cr == 0 && $(pass_n $d) == 2 ]]
  then ok "first call ${f} ms -> stock, later call ${lc} ms, 1 hang activate phase=preflight, late prepare stayed stock, no swap, no crash report (pid $pid)"
  else bad "firstcall ${f:-?} later ${lc:-?} hang=$h finished=$(cnt 'finished after' $d/dylib.log) swap=$(cnt 'swap:' $d/dylib.log) crashes=$cr pass=$(pass_n $d)"
  fi
  clear_sentinel $BP
}
c_act-swap-slow() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d f; n0=$(nl $e)
  run plain sws SIMCAM_TEST_SLOW_SWAP_MS=1500; d=$(res plain sws); f=$(fc $d)
  if in_range "$f" 1450 2000 && [[ $(pass_n $d) == 14 && $(since $e $n0 | grep -c 'hang activate') == 0 &&
    $(cnt 'COMMITTING overran' $d/dylib.log) == 1 ]]
  then ok "waiters returned after SWAP (${f} ms, not at 1000), armed 14/14, no hang, one COMMITTING-overran line"
  else bad "firstcall ${f:-?} pass=$(pass_n $d) hang=$(since $e $n0 | grep -c 'hang activate') overran=$(cnt 'COMMITTING overran' $d/dylib.log)"; fi
  clear_sentinel $BP
}
c_act-start-slow() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d f tsrc tor tarm tff tact
  n0=$(nl $e)
  run plain sts SIMCAM_TEST_SLOW_START_MS=5000 SIMCAM_SOURCE=image:$SIMCAM_DEV/media/qr.png SIMCAM_ORIENT=landscapeLeft; d=$(res plain sts); f=$(fc $d)
  tsrc=$(cfa 'source applied: image' $d/dylib.log); tor=$(cfa 'orientation -> 3 \(SIMCAM_ORIENT\)' $d/dylib.log)
  tarm=$(cfa 'LAZY armed by' $d/dylib.log)
  tff=$(cfa 'TIMING first preview frame' $d/dylib.log); tact=$(since $e $n0 | awk '$3=="activate"{print $1; exit}')
  local ob=$(since $e $n0 | awk -v a="$tact" '$3=="op-begin" && $4=="session" && $1-a<=1' | wc -l | tr -d ' ')
  if (( f < 100 )) && [[ -n $tsrc && -n $tor && -n $tarm && -n $tff && -n $tact ]] &&
    python3 -I -c "import sys; s,o,a,ff,act=map(float,sys.argv[1:]); sys.exit(0 if s<=ff+0.05 and o<a-3 and a-ff>3 and act-978307200<a-3 else 1)" $tsrc $tor $tarm $tff $tact &&
    (( ob >= 1 ))
  then ok "firstcall ${f} ms; still applied before the first frame, landscape forced at SWAP; activate + op-begin session at once; armed/flush line $(python3 -I -c "import sys;print('%.1f'%(float(sys.argv[1])-float(sys.argv[2])))" $tarm $tff) s after the first frame"
  else bad "fc=$f src=$tsrc orient=$tor armed=$tarm firstframe=$tff activate=$tact opbegin=$ob"; fi
  clear_sentinel $BP
}
c_act-control-slow() {
  clear_sentinel $BP; local C=$SIMCAM_RESULTS/activation-ctl-$TAG.txt img=$SIMCAM_DEV/media/qr.png
  printf 'source=image:%s\norientation=landscapeLeft\nevent=probeA@1\n' $img > $C
  bg_run plain cs SIMCAM_CONTROL=$C SIMCAM_TEST_SLOW_START_MS=5000
  wait_for $BGDIR/app.log 'START sleeps' 20 &&
    { sleep 1; printf 'source=image:%s\norientation=landscapeLeft\nevent=probeB@2\n' $img > $C.tmp; mv $C.tmp $C; }
  wait $BGPID; local d=$BGDIR
  if [[ $(cnt 'event probeA@1 present at start, not replayed' $d/dylib.log) == 1 && $(cnt 'unknown event probeA' $d/dylib.log) == 0 && $(cnt 'unknown event probeB' $d/dylib.log) == 1 &&
        $(cnt 'source applied: image' $d/dylib.log) -ge 1 && $(cnt 'orientation 3 forced at activation \(control file\)' $d/dylib.log) == 1 ]]
  then ok "control file's source/orientation applied at SWAP; event at the read not replayed; event written during START's sleep replayed once"
  else bad "atStart=$(cnt 'probeA@1 present' $d/dylib.log) replayA=$(cnt 'unknown event probeA' $d/dylib.log) replayB=$(cnt 'unknown event probeB' $d/dylib.log) src=$(cnt 'source applied: image' $d/dylib.log) orient=$(cnt 'forced at activation' $d/dylib.log)"
  fi
  rm -f $C; clear_sentinel $BP
}
c_act-override() {
  clear_sentinel $BP; local d f bad1= v i=0
  run plain ov0 SIMCAM_ACTIVATE_TIMEOUT_MS=200 SIMCAM_TEST_SLOW_ACTIVATE_MS=300 M5APP_EXPECT=stock; d=$(res plain ov0); f=$(fc $d)
  local r200="firstcall ${f:-?} ms, hang ms=$(grep -oE 'ms=[0-9]+ phase' "$(ev $BP)" | tail -1)"
  in_range "$f" 150 260 || bad1="200: $r200"
  clear_sentinel $BP
  for v in 0 -5 abc 999999; do
    i=$((i + 1)); run plain ov$i SIMCAM_ACTIVATE_TIMEOUT_MS=$v; d=$(res plain ov$i)
    [[ $(cnt "SIMCAM_ACTIVATE_TIMEOUT_MS=$v is not an integer" $d/app.log) == 1 &&
      $(cnt 'budget 1000 ms' $d/dylib.log) == 1 && $(pass_n $d) == 14 ]] ||
      bad1="$bad1 [$v: warn=$(cnt 'is not an integer' $d/app.log) budget1000=$(cnt 'budget 1000 ms' $d/dylib.log) pass=$(pass_n $d)]"
  done
  [[ -z $bad1 ]] && ok "200: $r200; 0, -5, abc, 999999: one stderr line at load each, budget 1000 ms, armed" || bad "$bad1"
  clear_sentinel $BP
}
# pause_case <tag> [K=V ...]: =800, kill -STOP 5 s during prepare. Sets PD (the run dir) and PPID_ (the pid it paused;
# empty if the gate line never came)
pause_case() {
  local n=$1; shift; clear_sentinel $BP
  bg_run plain $n SIMCAM_TEST_SLOW_ACTIVATE_MS=800 "$@"
  local pid=
  if wait_for $BGDIR/app.log 'TIMING gate entered' 20; then pid=$(app_pid $BGDIR/app.log); sleep 0.2; kill -STOP $pid; sleep 5; kill -CONT $pid; fi
  wait $BGPID; PD=$BGDIR; PPID_=$pid
}
c_act-pause() {
  pause_case pz; local d=$PD f=$(fc $PD)
  local ext=$(grep -oE 'deadline \+[0-9.]+ s' $d/dylib.log | tr '\n' ' ')
  if [[ -n $PPID_ && $(pass_n $d) == 14 && $(cnt 'deadline \+' $d/dylib.log) == 1 ]] && in_range "$f" 5500 6500
  then ok "armed 14/14; one '$ext'; first call ${f} ms"
  else bad "pid=$PPID_ pass=$(pass_n $d) ext='$ext' firstcall=${f:-?}"; fi
  clear_sentinel $BP
}
c_act-threads-pause() {
  pause_case tpz M5APP_ORDER=threads; local d=$PD
  local ext=$(grep -oE 'deadline \+[0-9.]+ s' $d/dylib.log | tr '\n' ' ')
  if [[ -n $PPID_ && $(cnt 'PASS order.threads ' $d/app.log) == 1 && $(cnt 'deadline \+' $d/dylib.log) == 1 &&
    $(pass_n $d) == 15 ]]
  then ok "armed, all 3 threads saw cameras ($(grep -oE 'results=\[[^]]*\]' $d/app.log)), exactly one '$ext'"
  else bad "pid=$PPID_ threads=$(cnt 'PASS order.threads ' $d/app.log) ext='$ext' pass=$(pass_n $d)"; fi
  clear_sentinel $BP
}
c_act-never() {
  clear_sentinel $BP; local e=$(ev $BP) n0 d; n0=$(nl $e)
  run plain nv SIMCAM_TEST_SLOW_ACTIVATE_MS=600000 M5APP_EXPECT=stock; d=$(res plain nv); sleep 1
  local h=$(since $e $n0 | grep -c 'hang activate') tw=$(cfa 'TIMING first camera call' $d/dylib.log) tr=$(cfa 'hang activate recorded' $d/dylib.log)
  if [[ $h == 1 && $(cnt DONE $d/app.log) == 1 && -n $tw && -n $tr ]] &&
    python3 -I -c "import sys; sys.exit(0 if abs(float(sys.argv[1])-float(sys.argv[2]))<1 else 1)" $tw $tr
  then ok "one hang activate line, recorded within 1 s of the timeout; app exited normally, no second line"
  else bad "hang=$h done=$(cnt DONE $d/app.log) timeout@$tw record@$tr"; fi
  clear_sentinel $BP
}
c_act-late() {
  clear_sentinel $BP; local e=$(ev $BP) n0 mx d; n0=$(nl $e); mx=$(maxact $e)
  # prepare ends 300 ms after the timeout (1050 left 50 ms, less than one 100 ms wait slice plus wake-up latency, for
  # the waiter's CAS to beat prepare's); still well inside the launch, so "finished after … staying stock" is logged
  run plain lt SIMCAM_TEST_SLOW_ACTIVATE_MS=1300 M5APP_EXPECT=stock; d=$(res plain lt)
  local h=$(since $e $n0 | grep -c 'hang activate') nums=$(since $e $n0 | awk '{print $2}' | sort -u | tr '\n' ' ')
  if [[ $h == 1 && $(cnt 'finished after .* staying stock' $d/dylib.log) == 1 && $(cnt 'swap:' $d/dylib.log) == 0 && $nums == "$(( mx + 1 )) " ]]
  then ok "one hang line, one 'finished after … staying stock', no swap; every line of this launch carries activation $(( mx + 1 ))"
  else bad "hang=$h finished=$(cnt 'finished after' $d/dylib.log) swap=$(cnt 'swap:' $d/dylib.log) activation numbers '$nums' (want $(( mx + 1 )))"
  fi
  clear_sentinel $BP
}
c_act-got() {
  clear_sentinel $BC; local d1 d2
  run canary got1 SIMCAM_SESSION_ROUTE=got; d1=$(res canary got1)
  local walk_after=$(awk '/LAZY armed by/{a=1; next} a && /got route:/' $d1/dylib.log | wc -l | tr -d ' ')
  # the timeout half runs in plain (M5APP_EXPECT=stock lives ~2 s): the canary quits at once with no camera, before the late
  # prepare's resolve step logs the slot
  clear_sentinel $BP; run plain got2 SIMCAM_SESSION_ROUTE=got SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d2=$(res plain got2); sleep 1
  local slot=$(grep -m1 -oE 'got route: .*prepare: found, not yet patched' $d2/dylib.log | cut -c1-120)
  if [[ $(cat $d1/summary.txt) == *core=PASS* && $(cnt 'prepare: found' $d1/dylib.log) -ge 1 && $walk_after == 0 && $(cnt 'entries rebound' $d1/dylib.log) == 1 &&
        $(pass_n $d2) == 2 && -n $slot && $(cnt 'entries rebound' $d2/dylib.log) == 0 ]]
  then ok "normal: $(grep -oE 'got, [0-9]+ of [0-9]+ entries rebound' $d1/dylib.log), canary core PASS, no image walk after ARMED; timeout: stock, slot unchanged ($slot)"
  else bad "normal: $(cut -c1-60 $d1/summary.txt) found=$(cnt 'prepare: found' $d1/dylib.log) walkAfter=$walk_after; timeout: pass=$(pass_n $d2) slot='$slot' rebound=$(cnt 'entries rebound' $d2/dylib.log)"
  fi
  clear_sentinel $BC; clear_sentinel $BP
}
c_act-main() {
  clear_sentinel $BP; local d1 d2 f
  run plain mn1 SIMCAM_TEST_ACTIVATE_NEEDS_MAIN=1 M5APP_EXPECT=stock; d1=$(res plain mn1); f=$(fc $d1)
  # bg caller: plain-orient coord-bg (first call on a camera queue; main is busy 300 ms, then free). plain's threads mode can't
  # be used: its main thread blocks in a group wait on the three threads, so prepare's dispatch_sync to main can never run.
  clear_sentinel $BO; p12 coord-bg landscapeLeft mn2 SIMCAM_TEST_ACTIVATE_NEEDS_MAIN=1; local d2=$PD
  if in_range "$f" 900 1150 && [[ $(cnt DONE $d1/app.log) == 1 && $(pass_n $d1) == 2 &&
    $(cnt 'LAZY armed by .* on bg thread' $d2/dylib.log) == 1 && $(cnt 'P12\| DONE' $d2/app.log) == 1 ]]
  then ok "main caller: stock at ${f} ms, no deadlock, app finished; bg caller (camera queue): armed ($(grep -oE 'firstcall\.ms=[0-9]+' $d2/app.log))"
  else bad "main: fc=$f done=$(cnt DONE $d1/app.log) pass=$(pass_n $d1); bg: armed=$(cnt 'LAZY armed by .* on bg thread' $d2/dylib.log) $(grep -oE 'firstcall\.ms=[0-9]+' $d2/app.log)"
  fi
  clear_sentinel $BP; clear_sentinel $BO
}
c_act-reenter() {
  clear_sentinel $BP; local d ms
  run plain re SIMCAM_TEST_REENTER=1; d=$(res plain re); ms=$(grep -oE 'LAZY armed by .* in [0-9.]+ ms' $d/dylib.log | grep -oE '[0-9.]+ ms$' | cut -d' ' -f1)
  if [[ $(pass_n $d) == 14 && $(cnt 're-entrant call init forwarded to stock' $d/dylib.log) == 1 ]] &&
    (( ${ms%.*} < 20 )); then ok "armed in ${ms} ms, 14/14; 're-entrant call init forwarded to stock (no wait)'"
  else bad "pass=$(pass_n $d) reenter=$(cnt 're-entrant call' $d/dylib.log) armed in ${ms:-?} ms"; fi
  clear_sentinel $BP
}
c_act-traced() {   # lldb attached (SB API, no GUI) from before the first instruction: budget x10; 11000 -> unknown-hang, not counted
  local py=/Applications/Xcode.app/Contents/Developer/usr/bin/python3 lp=$(xcrun lldb -P) e=$(ev $BP) n0 i d pid
  typeset -A F A
  clear_sentinel $BP; n0=$(nl $e)
  for i in 1500 11000; do
    local x=(SIMCAM_TEST_SLOW_ACTIVATE_MS=$i); [[ $i == 11000 ]] && x+=(M5APP_EXPECT=stock)
    ( XENV="${(F)x}"$'\n' SIMCAM_NO_RETRY=1 WAIT_DEBUGGER=1 $ROOT/scripts/run.sh plain $RT boot $TAG-tr$i >/dev/null 2>&1 ) & local rp=$!
    d=$(res plain tr$i); pid=
    # simctl prints the pid only once the app runs: take it from the host's process list (the path names this sim's UDID);
    # up to 10 s (200 polls of 0.05 s)
    for _ in {1..200}; do pid=$(pgrep -f "Devices/$U/data/Containers/Bundle/Application/.*/m5app.app/m5app" | head -1)
      [[ -n $pid ]] && break; sleep 0.05; done
    if [[ -n $pid ]]
    then PYTHONPATH=$lp $py $ROOT/scripts/lldb-attach.py $pid $d/app.log 'DONE' 40 > $SIMCAM_RESULTS/activation-$RT-$TAG-attach$i.txt 2>&1
    else echo "ATTACH| no pid" > $SIMCAM_RESULTS/activation-$RT-$TAG-attach$i.txt; fi
    wait $rp; F[$i]=$(fc $d)
    A[$i]=$(grep -m1 -oE 'attached pid [0-9]+ in [0-9.]+ s|attach FAILED.*|no pid' $SIMCAM_RESULTS/activation-$RT-$TAG-attach$i.txt)
  done
  local d1=$(res plain tr1500) d2=$(res plain tr11000) uh=$(since $e $n0 | grep -c 'unknown-hang activate') hg=$(since $e $n0 | grep -cE ' hang activate')
  local mk=$(nf $STATE/$BP/latched-*(N))
  if [[ ${A[1500]} == attached* && ${A[11000]} == attached* && $(pass_n $d1) == 14 &&
    $(cnt 'LAZY armed by' $d1/dylib.log) == 1 ]] && in_range "${F[1500]}" 1450 1700 &&
     in_range "${F[11000]}" 9800 10200 && [[ $(cnt DONE $d2/app.log) == 1 && $(pass_n $d2) == 2 && $uh == 1 && $hg == 0 && $mk == 0 ]]
  then ok "traced (${A[1500]}; ${A[11000]}): =1500 armed at ${F[1500]} ms (budget x10), 14/14; =11000 stock at ${F[11000]} ms, 1 'unknown-hang activate' (not counted: 0 hang lines), no marker"
  else bad "attach '${A[1500]}' / '${A[11000]}'; =1500 fc=${F[1500]:-?} pass=$(pass_n $d1) armed=$(cnt 'LAZY armed by' $d1/dylib.log); =11000 fc=${F[11000]:-?} done=$(cnt DONE $d2/app.log) unknown-hang=$uh hang=$hg markers=$mk"
  fi
  clear_sentinel $BP
}
c_mutation-L56() {   # SIMCAM_OFF_ACTIVATION_TIMEOUT=1 brings back the unbounded inline activation: act-slow-timeout must catch it
  # the run's tag is l53, not l56: it is part of the result dir's name (plain-<rt>-boot-<tag>-l53), so it stays
  clear_sentinel $BP; local d f; run plain l53 SIMCAM_OFF_ACTIVATION_TIMEOUT=1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d=$(res plain l53); f=$(fc $d)
  if (( ${f:-0} >= 1500 )) && [[ $(cnt 'inline: SIMCAM_OFF_ACTIVATION_TIMEOUT' $d/dylib.log) == 1 ]]
  then ok "mutant caught: the first call waited ${f} ms (act-slow-timeout wants 1000 ± 100), inline armed"
  else bad "mutant not caught: firstcall ${f:-?} ms"; fi
  clear_sentinel $BP
}
