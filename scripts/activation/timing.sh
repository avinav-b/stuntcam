# SPDX-License-Identifier: Apache-2.0
# activation/timing.sh — bounded activation: the first call's wait, timeouts, pauses,
# a late prepare, a debugger attached.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# CFAbsoluteTime (the engine log's stamps) counts seconds from 2001-01-01, which is this many seconds after the Unix
# epoch
CF_EPOCH_OFFSET_S=978307200

c_act-slow-ok() {
  clear_sentinel $PLAIN_BUNDLE

  run plain so SIMCAM_TEST_SLOW_ACTIVATE_MS=300
  local dir=$(run_dir plain so)
  local first_ms
  first_ms=$(first_call_ms $dir)

  # the first call takes at least prepare's 300 ms sleep, and at most 150 ms more for the rest of the activation
  local min_ms=300
  local max_ms=450
  local pass=$(pass_count $dir)
  local armed=$(count_matches 'LAZY armed by' $dir/dylib.log)

  if in_range "$first_ms" $min_ms $max_ms && [[ $pass == $PLAIN_CHECKS && $armed == 1 ]]; then
    ok "firstcall ${first_ms} ms, 14/14, one armed-by line"
  else
    bad "firstcall ${first_ms:-?} ms, pass $pass, armed-by lines $armed"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-slow-timeout() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen dir first_ms later_ms hangs reports pid
  events_seen=$(line_count $events)
  run plain st SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  dir=$(run_dir plain st)
  first_ms=$(first_call_ms $dir)
  later_ms=$(later_call_ms $dir)
  pid=$(fault_pid $dir/app.log)
  crash_reports $PLAIN_PROCESS 0 $pid
  reports=$REPORTS_FOUND
  [[ -n $pid ]] || reports=no-pid
  hangs=$(lines_after $events $events_seen | grep -c 'hang activate .* phase=preflight')
  local done_lines=$(count_matches DONE $dir/app.log)
  local stayed_stock=$(count_matches 'finished after .* staying stock' $dir/dylib.log)
  local swap=$(count_matches 'swap:' $dir/dylib.log)
  local pass=$(pass_count $dir)

  # the first call times out at 1000 ms (± 100); a later call returns at once (under 5 ms)
  local timeout_min_ms=900
  local timeout_max_ms=1100
  local later_call_max_ms=5
  if in_range "$first_ms" $timeout_min_ms $timeout_max_ms && (( ${later_ms%.*} < later_call_max_ms )) &&
    [[ $done_lines == 1 && $stayed_stock == 1 && $swap == 0 && $hangs == 1 && $reports == 0 && $pass == 2 ]]; then
    ok "first call ${first_ms} ms -> stock, later call ${later_ms} ms, 1 hang activate phase=preflight, late prepare" \
      "stayed stock," \
      "no swap, no crash report (pid $pid)"
  else
    bad "firstcall ${first_ms:-?} later ${later_ms:-?} hang=$hangs" \
      "finished=$(count_matches 'finished after' $dir/dylib.log) swap=$swap" \
      "crashes=$reports pass=$pass"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-swap-slow() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen dir first_ms
  events_seen=$(line_count $events)
  run plain sws SIMCAM_TEST_SLOW_SWAP_MS=1500
  dir=$(run_dir plain sws)
  first_ms=$(first_call_ms $dir)
  local pass=$(pass_count $dir)
  local hangs=$(lines_after $events $events_seen | grep -c 'hang activate')
  local overran=$(count_matches 'COMMITTING overran' $dir/dylib.log)

  # the waiters return when SWAP ends (its 1500 ms sleep and a little more), not at the 1000 ms budget
  local swap_end_min_ms=1450
  local swap_end_max_ms=2000
  if in_range "$first_ms" $swap_end_min_ms $swap_end_max_ms &&
    [[ $pass == $PLAIN_CHECKS && $hangs == 0 && $overran == 1 ]]; then
    ok "waiters returned after SWAP (${first_ms} ms, not at 1000), armed 14/14, no hang, one COMMITTING-overran line"
  else
    bad "firstcall ${first_ms:-?} pass=$pass hang=$hangs overran=$overran"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# session_begins_within_1s <events.log> <n> <epoch>: how many "op-begin session" lines after the first <n> are at
# most 1 s after <epoch>
session_begins_within_1s() {
  lines_after $1 $2 | awk -v a="$3" '$3=="op-begin" && $4=="session" && $1-a<=1' | wc -l | tr -d ' '
}

c_act-start-slow() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen dir first_ms source_at orient_at armed_at first_frame_at activate_epoch
  events_seen=$(line_count $events)
  run plain sts SIMCAM_TEST_SLOW_START_MS=5000 SIMCAM_SOURCE=image:$SIMCAM_DEV/media/qr.png \
    SIMCAM_ORIENT=landscapeLeft
  dir=$(run_dir plain sts)
  first_ms=$(first_call_ms $dir)

  # When the source was applied, the orientation forced, the armed line logged and the first frame shown (all
  # CFAbsoluteTime), and the time of the activate events line (epoch seconds)
  source_at=$(first_cf_time 'source applied: image' $dir/dylib.log)
  orient_at=$(first_cf_time 'orientation -> 3 \(SIMCAM_ORIENT\)' $dir/dylib.log)
  armed_at=$(first_cf_time 'LAZY armed by' $dir/dylib.log)
  first_frame_at=$(first_cf_time 'TIMING first preview frame' $dir/dylib.log)
  activate_epoch=$(first_event_time $events $events_seen activate)

  # op-begin session lines within 1 s of the activate line
  local session_begins=$(session_begins_within_1s $events $events_seen "$activate_epoch")

  # The source must be applied before the first frame (with 50 ms of slack). The orientation must be forced, and the
  # activate line written, more than 3 s (START's sleep) before the armed line. The armed line must come more than
  # 3 s after the first frame.
  local order_check="
import sys
source_at, orient_at, armed_at, first_frame_at, activate_epoch = map(float, sys.argv[1:])
activate_at = activate_epoch - $CF_EPOCH_OFFSET_S
source_first = source_at <= first_frame_at + 0.05
start_slept = orient_at < armed_at - 3 and armed_at - first_frame_at > 3 and activate_at < armed_at - 3
sys.exit(0 if source_first and start_slept else 1)"
  local seconds_between='
import sys
print("%.1f" % (float(sys.argv[1]) - float(sys.argv[2])))'

  local first_call_max_ms=100   # START's sleep is after the first call returns
  if (( first_ms < first_call_max_ms )) && [[ -n $source_at && -n $orient_at && -n $armed_at && -n $first_frame_at &&
    -n $activate_epoch ]] &&
    python3 -I -c "$order_check" $source_at $orient_at $armed_at $first_frame_at $activate_epoch &&
    (( session_begins >= 1 )); then
    local armed_after_s=$(python3 -I -c "$seconds_between" $armed_at $first_frame_at)
    ok "firstcall ${first_ms} ms; still applied before the first frame, landscape forced at SWAP;" \
      "activate + op-begin session at once; armed/flush line $armed_after_s s after the first frame"
  else
    bad "fc=$first_ms src=$source_at orient=$orient_at armed=$armed_at firstframe=$first_frame_at" \
      "activate=$activate_epoch opbegin=$session_begins"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-control-slow() {
  clear_sentinel $PLAIN_BUNDLE

  local control=$SIMCAM_RESULTS/activation-ctl-$TAG.txt
  local image=$SIMCAM_DEV/media/qr.png
  printf 'source=image:%s\norientation=landscapeLeft\nevent=probeA@1\n' $image > $control
  bg_run plain cs SIMCAM_CONTROL=$control SIMCAM_TEST_SLOW_START_MS=5000

  if wait_for $BG_DIR/app.log 'START sleeps' 20; then
    # a new event written (atomically) during START's sleep
    sleep 1
    printf 'source=image:%s\norientation=landscapeLeft\nevent=probeB@2\n' $image > $control.tmp
    mv $control.tmp $control
  fi

  wait $BG_PID
  local dir=$BG_DIR
  local at_start=$(count_matches 'event probeA@1 present at start, not replayed' $dir/dylib.log)
  local replay_a=$(count_matches 'unknown event probeA' $dir/dylib.log)
  local replay_b=$(count_matches 'unknown event probeB' $dir/dylib.log)
  local src=$(count_matches 'source applied: image' $dir/dylib.log)
  local orient=$(count_matches 'orientation 3 forced at activation \(control file\)' $dir/dylib.log)

  if [[ $at_start == 1 && $replay_a == 0 && $replay_b == 1 && $src -ge 1 && $orient == 1 ]]; then
    ok "control file's source/orientation applied at SWAP; event at the read not replayed;" \
      "event written during START's sleep replayed once"
  else
    bad "atStart=$(count_matches 'probeA@1 present' $dir/dylib.log) replayA=$replay_a replayB=$replay_b src=$src" \
      "orient=$(count_matches 'forced at activation' $dir/dylib.log)"
  fi

  rm -f $control
  clear_sentinel $PLAIN_BUNDLE
}

c_act-override() {
  clear_sentinel $PLAIN_BUNDLE

  local failures=
  local i=0
  local dir first_ms value
  run plain ov0 SIMCAM_ACTIVATE_TIMEOUT_MS=200 SIMCAM_TEST_SLOW_ACTIVATE_MS=300 M5APP_EXPECT=stock
  dir=$(run_dir plain ov0)
  first_ms=$(first_call_ms $dir)
  local hang_ms=$(grep -oE 'ms=[0-9]+ phase' "$(events_log $PLAIN_BUNDLE)" | tail -1)
  local result_200="firstcall ${first_ms:-?} ms, hang ms=$hang_ms"
  # a 200 ms budget: the first call returns at 200 ms (-50, +60)
  local budget_min_ms=150
  local budget_max_ms=260
  in_range "$first_ms" $budget_min_ms $budget_max_ms || failures="200: $result_200"
  clear_sentinel $PLAIN_BUNDLE

  # values that are not a usable timeout: each gives one stderr line at load and the default 1000 ms budget
  for value in 0 -5 abc 999999; do
    i=$((i + 1))
    run plain ov$i SIMCAM_ACTIVATE_TIMEOUT_MS=$value
    dir=$(run_dir plain ov$i)
    local warned=$(count_matches "SIMCAM_ACTIVATE_TIMEOUT_MS=$value is not an integer" $dir/app.log)
    local budget1000=$(count_matches 'budget 1000 ms' $dir/dylib.log)
    local pass=$(pass_count $dir)
    if ! [[ $warned == 1 && $budget1000 == 1 && $pass == $PLAIN_CHECKS ]]; then
      failures+=" [$value: warn=$(count_matches 'is not an integer' $dir/app.log)"
      failures+=" budget1000=$budget1000 pass=$pass]"
    fi
  done

  if [[ -z $failures ]]; then
    ok "200: $result_200; 0, -5, abc, 999999: one stderr line at load each, budget 1000 ms, armed"
  else
    bad "$failures"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# pause_case <tag> [K=V ...]: one launch of plain with an 800 ms prepare, stopped with kill -STOP for 5 s while the
# prepare runs. Sets LAST_RUN_DIR to the run dir and PAUSED_PID to the pid it paused (empty if the gate line never
# came).
pause_case() {
  local tag=$1
  shift
  clear_sentinel $PLAIN_BUNDLE
  bg_run plain $tag SIMCAM_TEST_SLOW_ACTIVATE_MS=800 "$@"
  local pid=

  if wait_for $BG_DIR/app.log 'TIMING gate entered' 20; then
    pid=$(app_pid $BG_DIR/app.log)
    sleep 0.2
    kill -STOP $pid
    sleep 5
    kill -CONT $pid
  fi

  wait $BG_PID
  LAST_RUN_DIR=$BG_DIR
  PAUSED_PID=$pid
}

c_act-pause() {
  pause_case pz
  local dir=$LAST_RUN_DIR
  local first_ms=$(first_call_ms $LAST_RUN_DIR)
  local extension=$(grep -oE 'deadline \+[0-9.]+ s' $dir/dylib.log | tr '\n' ' ')
  local pass=$(pass_count $dir)
  local extensions=$(count_matches 'deadline \+' $dir/dylib.log)

  # the 5 s pause extends the deadline once: the first call returns after the pause plus prepare's 800 ms
  local resumed_min_ms=5500
  local resumed_max_ms=6500
  if [[ -n $PAUSED_PID && $pass == $PLAIN_CHECKS && $extensions == 1 ]] &&
    in_range "$first_ms" $resumed_min_ms $resumed_max_ms; then
    ok "armed 14/14; one '$extension'; first call ${first_ms} ms"
  else
    bad "pid=$PAUSED_PID pass=$pass ext='$extension' firstcall=${first_ms:-?}"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-threads-pause() {
  pause_case tpz M5APP_ORDER=threads
  local dir=$LAST_RUN_DIR
  local extension=$(grep -oE 'deadline \+[0-9.]+ s' $dir/dylib.log | tr '\n' ' ')
  local threads=$(count_matches 'PASS order.threads ' $dir/app.log)
  local extensions=$(count_matches 'deadline \+' $dir/dylib.log)
  local pass=$(pass_count $dir)

  if [[ -n $PAUSED_PID && $threads == 1 && $extensions == 1 && $pass == "$ORDER_RUN_CHECKS" ]]; then
    ok "armed, all 3 threads saw cameras ($(grep -oE 'results=\[[^]]*\]' $dir/app.log)), exactly one '$extension'"
  else
    bad "pid=$PAUSED_PID threads=$threads ext='$extension' pass=$pass"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-never() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen dir
  events_seen=$(line_count $events)
  run plain nv SIMCAM_TEST_SLOW_ACTIVATE_MS=600000 M5APP_EXPECT=stock
  dir=$(run_dir plain nv)
  sleep 1
  local hangs=$(lines_after $events $events_seen | grep -c 'hang activate')
  local timeout_at=$(first_cf_time 'TIMING first camera call' $dir/dylib.log)
  local record_at=$(first_cf_time 'hang activate recorded' $dir/dylib.log)
  local done_lines=$(count_matches DONE $dir/app.log)
  local within_1s='
import sys
sys.exit(0 if abs(float(sys.argv[1]) - float(sys.argv[2])) < 1 else 1)'

  if [[ $hangs == 1 && $done_lines == 1 && -n $timeout_at && -n $record_at ]] &&
    python3 -I -c "$within_1s" $timeout_at $record_at; then
    ok "one hang activate line, recorded within 1 s of the timeout; app exited normally, no second line"
  else
    bad "hang=$hangs done=$done_lines timeout@$timeout_at record@$record_at"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# activation_numbers <events.log> <n>: the distinct activation numbers (second field) after the first <n> lines,
# sorted, each followed by a space
activation_numbers() { lines_after $1 $2 | awk '{print $2}' | sort -u | tr '\n' ' '; }

c_act-late() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen max_number dir
  events_seen=$(line_count $events)
  max_number=$(max_activation_number $events)

  # Prepare ends 300 ms after the timeout. (A 1050 ms prepare left only 50 ms for the waiter's CAS to beat
  # prepare's, less than one 100 ms wait slice plus wake-up latency.) That is still well inside the launch, so
  # "finished after … staying stock" is logged.
  run plain lt SIMCAM_TEST_SLOW_ACTIVATE_MS=1300 M5APP_EXPECT=stock
  dir=$(run_dir plain lt)
  local hangs=$(lines_after $events $events_seen | grep -c 'hang activate')
  local numbers=$(activation_numbers $events $events_seen)
  local stayed_stock=$(count_matches 'finished after .* staying stock' $dir/dylib.log)
  local swap=$(count_matches 'swap:' $dir/dylib.log)

  if [[ $hangs == 1 && $stayed_stock == 1 && $swap == 0 && $numbers == "$(( max_number + 1 )) " ]]; then
    ok "one hang line, one 'finished after … staying stock', no swap;" \
      "every line of this launch carries activation $(( max_number + 1 ))"
  else
    bad "hang=$hangs finished=$(count_matches 'finished after' $dir/dylib.log) swap=$swap" \
      "activation numbers '$numbers' (want $(( max_number + 1 )))"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# got_routes_after_armed <dylib.log>: how many "got route:" lines come after the "LAZY armed by" line
got_routes_after_armed() { awk '/LAZY armed by/{a=1; next} a && /got route:/' $1 | wc -l | tr -d ' '; }

c_act-got() {
  clear_sentinel $CANARY_BUNDLE

  local dir1 dir2
  run canary got1 SIMCAM_SESSION_ROUTE=got
  dir1=$(run_dir canary got1)

  # "got route:" lines after the armed line would mean an image walk after ARMED
  local walk_after=$(got_routes_after_armed $dir1/dylib.log)

  # The timeout half runs in plain, which lives about 2 s with M5APP_EXPECT=stock. The canary quits at once when it
  # has no camera, before the late prepare's resolve step logs the slot.
  clear_sentinel $PLAIN_BUNDLE
  run plain got2 SIMCAM_SESSION_ROUTE=got SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  dir2=$(run_dir plain got2)
  sleep 1
  local slot=$(grep -m1 -oE 'got route: .*prepare: found, not yet patched' $dir2/dylib.log | cut -c1-120)
  local found=$(count_matches 'prepare: found' $dir1/dylib.log)
  local rebound=$(count_matches 'entries rebound' $dir1/dylib.log)
  local timeout_pass=$(pass_count $dir2)
  local timeout_rebound=$(count_matches 'entries rebound' $dir2/dylib.log)
  local canary_summary=$(cat $dir1/summary.txt)

  if [[ $canary_summary == *core=PASS* && $found -ge 1 && $walk_after == 0 && $rebound == 1 ]] &&
    [[ $timeout_pass == 2 && -n $slot && $timeout_rebound == 0 ]]; then
    ok "normal: $(grep -oE 'got, [0-9]+ of [0-9]+ entries rebound' $dir1/dylib.log), canary core PASS," \
      "no image walk after ARMED; timeout: stock, slot unchanged ($slot)"
  else
    bad "normal: $(cut -c1-60 $dir1/summary.txt) found=$found walkAfter=$walk_after;" \
      "timeout: pass=$timeout_pass slot='$slot' rebound=$timeout_rebound"
  fi

  clear_sentinel $CANARY_BUNDLE
  clear_sentinel $PLAIN_BUNDLE
}

c_act-main() {
  clear_sentinel $PLAIN_BUNDLE

  local dir1 dir2 first_ms
  run plain mn1 SIMCAM_TEST_ACTIVATE_NEEDS_MAIN=1 M5APP_EXPECT=stock
  dir1=$(run_dir plain mn1)
  first_ms=$(first_call_ms $dir1)

  # The background caller is plain-orient's coord-bg mode: its first call comes on a camera queue while main is busy
  # for 300 ms, then free. plain's threads mode can't be used: its main thread blocks in a group wait on the three
  # threads, so prepare's dispatch_sync to main could never run.
  clear_sentinel $ORIENT_BUNDLE
  run_plain_orient coord-bg landscapeLeft mn2 SIMCAM_TEST_ACTIVATE_NEEDS_MAIN=1
  local dir2=$LAST_RUN_DIR
  local done_lines=$(count_matches DONE $dir1/app.log)
  local pass=$(pass_count $dir1)
  local bg_armed=$(count_matches 'LAZY armed by .* on bg thread' $dir2/dylib.log)
  local bg_done=$(count_matches 'P12\| DONE' $dir2/app.log)

  # the main caller goes stock at the 1000 ms budget (-100, +150) instead of deadlocking
  local timeout_min_ms=900
  local timeout_max_ms=1150
  if in_range "$first_ms" $timeout_min_ms $timeout_max_ms &&
    [[ $done_lines == 1 && $pass == 2 && $bg_armed == 1 && $bg_done == 1 ]]; then
    ok "main caller: stock at ${first_ms} ms, no deadlock, app finished;" \
      "bg caller (camera queue): armed ($(grep -oE 'firstcall\.ms=[0-9]+' $dir2/app.log))"
  else
    bad "main: fc=$first_ms done=$done_lines pass=$pass;" \
      "bg: armed=$bg_armed $(grep -oE 'firstcall\.ms=[0-9]+' $dir2/app.log)"
  fi

  clear_sentinel $PLAIN_BUNDLE
  clear_sentinel $ORIENT_BUNDLE
}

# armed_ms <run dir>: how long the activation took, from the engine's "LAZY armed by ... in <ms> ms" line
armed_ms() { grep -oE 'LAZY armed by .* in [0-9.]+ ms' $1/dylib.log | grep -oE '[0-9.]+ ms$' | cut -d' ' -f1; }

c_act-reenter() {
  clear_sentinel $PLAIN_BUNDLE

  local dir armed_in_ms
  run plain re SIMCAM_TEST_REENTER=1
  dir=$(run_dir plain re)
  armed_in_ms=$(armed_ms $dir)
  local pass=$(pass_count $dir)
  local forwarded=$(count_matches 're-entrant call init forwarded to stock' $dir/dylib.log)

  local armed_max_ms=20   # the re-entrant call is forwarded at once, so the activation doesn't wait for it
  if [[ $pass == $PLAIN_CHECKS && $forwarded == 1 ]] && (( ${armed_in_ms%.*} < armed_max_ms )); then
    ok "armed in ${armed_in_ms} ms, 14/14; 're-entrant call init forwarded to stock (no wait)'"
  else
    bad "pass=$pass reenter=$(count_matches 're-entrant call' $dir/dylib.log) armed in ${armed_in_ms:-?} ms"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# With lldb attached (the SB API, no GUI) from before the first instruction, the budget is 10 times longer. A
# prepare of 11000 ms then ends as an unknown-hang, which is not counted.
c_act-traced() {
  local xcode_python=/Applications/Xcode.app/Contents/Developer/usr/bin/python3
  local lldb_path=$(xcrun lldb -P)
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen i dir pid
  typeset -A first_ms attach
  clear_sentinel $PLAIN_BUNDLE
  events_seen=$(line_count $events)

  for i in 1500 11000; do
    local env_list=(SIMCAM_TEST_SLOW_ACTIVATE_MS=$i)
    [[ $i == 11000 ]] && env_list+=(M5APP_EXPECT=stock)
    # like bg_run, with WAIT_DEBUGGER=1 (XENV as in run)
    ( XENV="${(F)env_list}"$'\n' SIMCAM_NO_RETRY=1 WAIT_DEBUGGER=1 $ROOT/scripts/run.sh plain $RUNTIME boot $TAG-tr$i \
      >/dev/null 2>&1 ) &
    local run_pid=$!
    dir=$(run_dir plain tr$i)
    pid=
    # simctl prints the pid only once the app runs, so take it from the host's process list (the path names this
    # sim's UDID). Poll for up to 10 s (200 polls, 0.05 s apart; wd-traced.sh names the same loop PID_POLLS and
    # PID_POLL_INTERVAL, and its ATTACH_TIMEOUT_S is the 40 below).
    for _ in {1..200}; do
      pid=$(pgrep -f "Devices/$UDID/data/Containers/Bundle/Application/.*/$PLAIN_PROCESS.app/$PLAIN_PROCESS" | head -1)
      [[ -n $pid ]] && break
      sleep 0.05
    done
    local attach_log=$SIMCAM_RESULTS/activation-$RUNTIME-$TAG-attach$i.txt
    if [[ -n $pid ]]; then
      PYTHONPATH=$lldb_path $xcode_python $ROOT/scripts/lldb-attach.py $pid $dir/app.log 'DONE' 40 > $attach_log 2>&1
    else
      echo "ATTACH| no pid" > $attach_log
    fi
    wait $run_pid
    first_ms[$i]=$(first_call_ms $dir)
    attach[$i]=$(grep -m1 -oE 'attached pid [0-9]+ in [0-9.]+ s|attach FAILED.*|no pid' $attach_log)
  done

  local run1500=$(run_dir plain tr1500)
  local run11000=$(run_dir plain tr11000)
  local unknown_hangs=$(lines_after $events $events_seen | grep -c 'unknown-hang activate')
  local hangs=$(lines_after $events $events_seen | grep -cE ' hang activate')
  local markers=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N))
  local pass1=$(pass_count $run1500)
  local pass2=$(pass_count $run11000)
  local armed1=$(count_matches 'LAZY armed by' $run1500/dylib.log)
  local done2=$(count_matches DONE $run11000/app.log)
  # traced, the budget is 10x (10 s): a 1500 ms prepare still arms, an 11000 ms one goes stock at about 10 s
  local armed_min_ms=1450
  local armed_max_ms=1700
  local stock_min_ms=9800
  local stock_max_ms=10200

  if [[ ${attach[1500]} == attached* && ${attach[11000]} == attached* && $pass1 == $PLAIN_CHECKS && $armed1 == 1 ]] &&
    in_range "${first_ms[1500]}" $armed_min_ms $armed_max_ms &&
    in_range "${first_ms[11000]}" $stock_min_ms $stock_max_ms &&
    [[ $done2 == 1 && $pass2 == 2 && $unknown_hangs == 1 && $hangs == 0 && $markers == 0 ]]; then
    ok "traced (${attach[1500]}; ${attach[11000]}): =1500 armed at ${first_ms[1500]} ms (budget x10), 14/14;" \
      "=11000 stock at ${first_ms[11000]} ms, 1 'unknown-hang activate' (not counted: 0 hang lines), no marker"
  else
    bad "attach '${attach[1500]}' / '${attach[11000]}'; =1500 fc=${first_ms[1500]:-?} pass=$pass1 armed=$armed1;" \
      "=11000 fc=${first_ms[11000]:-?} done=$done2 unknown-hang=$unknown_hangs hang=$hangs markers=$markers"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_ACTIVATION_TIMEOUT=1 brings back the unbounded inline activation. act-slow-timeout must catch it.
c_mutation-L56() {
  # The run's tag is l53, not l56: it is part of the result dir's name (plain-<rt>-boot-<tag>-l53), so it stays.
  clear_sentinel $PLAIN_BUNDLE
  local dir first_ms
  run plain l53 SIMCAM_OFF_ACTIVATION_TIMEOUT=1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  dir=$(run_dir plain l53)
  first_ms=$(first_call_ms $dir)

  local slow_prepare_ms=1500   # with no timeout the first call waits out the whole slow prepare
  if (( ${first_ms:-0} >= slow_prepare_ms )) &&
    [[ $(count_matches 'inline: SIMCAM_OFF_ACTIVATION_TIMEOUT' $dir/dylib.log) == 1 ]]; then
    ok "mutant caught: the first call waited ${first_ms} ms (act-slow-timeout wants 1000 ± 100), inline armed"
  else
    bad "mutant not caught: firstcall ${first_ms:-?} ms"
  fi

  clear_sentinel $PLAIN_BUNDLE
}
