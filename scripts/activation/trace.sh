# SPDX-License-Identifier: Apache-2.0
# activation/trace.sh — the op trace file and a start's flush of a deferred stop.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# ---------------------------------------------------------------- op trace file, deferred-stop flush
# These cases test the op trace's file (core/trace.inc) and a start's flush of a deferred stop (fig/session_state.inc).
# They read traces with the CLI (`stuntcam trace --json --pid`), the same parser that `stuntcam trace` users get.
CLI_BIN=$ROOT/cli/bin/stuntcam

# trace_json <pid> <out.json>: writes the CLI's JSON trace of that pid of the plain app to <out.json>, and the CLI's
# stderr to <out.json>.err. Returns the CLI's exit status.
trace_json() { $CLI_BIN trace --json --udid $UDID --pid $1 $PLAIN_BUNDLE > $2 2>$2.err; }

# trace_summary <json>: "<location> <n ops> <n crash ops> <ms from activate to the first crash op | ->
# <ops joined by ,>", or "unparseable"
trace_summary() {
  # a missing or torn trace makes Python fail: that is the "unparseable" answer, not an error to show
  python3 -I -c '
import json, sys
trace = json.load(open(sys.argv[1]))
ops = trace["ops"]
names = [op["op"] for op in ops]
activates = [op for op in ops if op["op"] == "activate"]
crashes = [op for op in ops if op["op"] == "crash"]
if activates and crashes:
    ms = "%.1f" % ((crashes[0]["wall_ns"] - activates[0]["wall_ns"]) / 1e6)
else:
    ms = "-"
print(trace["location"], len(names), names.count("crash"), ms, ",".join(names))' $1 2>/dev/null || echo "unparseable"
}

# launch_pid <app.log>: the pid(s) on the launch's first "[simcam <pid>]" line
launch_pid() { grep -m1 -oE '\[simcam [0-9]+\]' $1 2>/dev/null | grep -oE '[0-9]+'; }

# trace_crash <tag> <SIMCAM_TEST_ACTIVATE_FAIL value> [K=V ...]: one launch of plain that faults in START. Sets these
# globals:
#   TRACE_PID           the pid of the launch's fault line (empty if there is none)
#   TRACE_CLI_RC        the exit status of the CLI that read the trace ("x" if there was no pid to ask for)
#   TRACE_SUMMARY       trace_summary of that trace (or why there is none)
#   TRACE_CRASH_LINES   how many crash lines the launch added to events.log
#   TRACE_TEMP_FILES    how many temp trace files of that pid were left behind
trace_crash() {
  local tag=$1
  local fail=$2
  shift 2
  clear_sentinel $PLAIN_BUNDLE
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen
  events_seen=$(line_count $events)
  run plain $tag SIMCAM_TEST_ACTIVATE_FAIL=$fail "$@"
  sleep 1
  local dir=$(run_dir plain $tag)
  TRACE_PID=$(fault_pid $dir/app.log 'SIMCAM_TEST_ACTIVATE_FAIL=')
  TRACE_CRASH_LINES=$(lines_after $events $events_seen | grep -c ' crash')

  if [[ -z $TRACE_PID ]]; then
    TRACE_CLI_RC=x
    TRACE_SUMMARY="no fault line in $dir/app.log"
    TRACE_TEMP_FILES=0
    clear_sentinel $PLAIN_BUNDLE
    return
  fi

  trace_json $TRACE_PID $dir/trace.json
  TRACE_CLI_RC=$?
  TRACE_SUMMARY=$(trace_summary $dir/trace.json)
  TRACE_TEMP_FILES=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/.trace-$TRACE_PID.*(N) \
    $(fallback_state_dir $PLAIN_BUNDLE)/.trace-$TRACE_PID.*(N))
  clear_sentinel $PLAIN_BUNDLE
}

# A crash in START, before the first 150 ms flush, still leaves the trace: the crash handler writes it.
c_trace-crash-start() {
  trace_crash tcs crashstart
  local -a fields
  fields=(${=TRACE_SUMMARY})   # location, ops, crash ops, ms after activate, op list

  if [[ $TRACE_CLI_RC == 0 && ${fields[3]} == 1 && $TRACE_SUMMARY == *activate*crash* && $TRACE_CRASH_LINES == 1 &&
    $TRACE_TEMP_FILES == 0 ]]; then
    ok "pid $TRACE_PID: trace written by the crash handler, parsed by the CLI: ${fields[1]}, ${fields[2]} ops," \
      "crash ${fields[4]} ms after activate (${fields[5]}); 1 crash line"
  else
    # if there is no .err file, the text in parentheses is just empty
    bad "pid ${TRACE_PID:-?}: CLI rc=$TRACE_CLI_RC trace '$TRACE_SUMMARY' crash lines=$TRACE_CRASH_LINES temp files" \
      "left=$TRACE_TEMP_FILES" \
      "($(head -c 200 "$(run_dir plain tcs)/trace.json.err" 2>/dev/null))"
  fi
}

# Mutant SIMCAM_OFF_TRACE_DIR_AT_ARM=1 picks the trace directory at the first flush again, so the same crash leaves
# no trace.
c_mutation-L69() {
  trace_crash tcl crashstart SIMCAM_OFF_TRACE_DIR_AT_ARM=1

  if [[ $TRACE_CLI_RC != 0 && $TRACE_CRASH_LINES == 1 ]]; then
    # if there is no .err file, the text in parentheses is just empty
    ok "mutant caught: pid $TRACE_PID crashed (1 crash line) and left no trace" \
      "(CLI rc=$TRACE_CLI_RC: $(head -c 120 "$(run_dir plain tcl)/trace.json.err" 2>/dev/null))"
  else
    bad "mutant not caught: pid ${TRACE_PID:-?} CLI rc=$TRACE_CLI_RC trace '$TRACE_SUMMARY' crash" \
      "lines=$TRACE_CRASH_LINES"
  fi
}

# crash2_seq <tag> [K=V ...]: launches of plain where two threads crash at once (SIMCAM_TEST_ACTIVATE_FAIL=crash2).
# It stops after 6 launches, or sooner once 3 launches had both threads in the crash handler (2 crash lines). Under
# heavy load the second thread may not run before the process ends; such a launch counts only for the whole-trace check.
# With DUAL_CRASH_MUTANT=1 (the mutant run) it makes up to 10 launches and stops at the first one that had both threads
# in the handler and left no whole trace. Such a catch needs the fault line (so the CLI ran) and 2 crash lines: a
# launch without them can't have raced, so its broken trace is no catch. Sets these globals:
#   DUAL_CRASH_LAUNCHES   how many launches it made
#   DUAL_CRASH_BOTH       how many of them had both threads in the handler
#   DUAL_CRASH_WHOLE      how many of them left a whole trace
#   DUAL_CRASH_CAUGHT     1 if the mutant run found its catch, else 0
#   DUAL_CRASH_DETAILS    each launch's details, for the verdict line
crash2_seq() {
  local tag=$1
  local mutant=${DUAL_CRASH_MUTANT:-0}
  local i
  shift
  DUAL_CRASH_LAUNCHES=0
  DUAL_CRASH_BOTH=0
  DUAL_CRASH_WHOLE=0
  DUAL_CRASH_CAUGHT=0
  DUAL_CRASH_DETAILS=

  for i in {1..$(( mutant ? 10 : 6 ))}; do
    (( ! mutant && DUAL_CRASH_BOTH >= 3 )) && break
    trace_crash $tag-$i crash2 "$@"
    DUAL_CRASH_LAUNCHES=$((DUAL_CRASH_LAUNCHES + 1))
    local -a fields
    fields=(${=TRACE_SUMMARY})
    # whole: the CLI parsed it, it has a crash op, and no temp file was left
    local whole=0
    if [[ $TRACE_CLI_RC == 0 && ${fields[3]:-0} -ge 1 && $TRACE_TEMP_FILES == 0 ]]; then
      whole=1
      DUAL_CRASH_WHOLE=$((DUAL_CRASH_WHOLE + 1))
    fi
    [[ $TRACE_CRASH_LINES == 2 ]] && DUAL_CRASH_BOTH=$((DUAL_CRASH_BOTH + 1))
    DUAL_CRASH_DETAILS+=" [pid $TRACE_PID rc=$TRACE_CLI_RC crash lines=$TRACE_CRASH_LINES"
    DUAL_CRASH_DETAILS+=" crash ops=${fields[3]:-?} ops=${fields[2]:-?} tmp=$TRACE_TEMP_FILES]"
    if (( mutant && ! whole )) && [[ $TRACE_CLI_RC != x && $TRACE_CRASH_LINES == 2 ]]; then
      DUAL_CRASH_CAUGHT=1
      break
    fi
  done
}

# With two threads in the crash handler at once there is still one whole trace, which the CLI parses. The case
# needs 3 launches in which both threads ran the handler, in at most 6 launches. Any launch without a whole trace
# FAILs.
c_trace-crash2() {
  crash2_seq tc2

  if (( DUAL_CRASH_WHOLE == DUAL_CRASH_LAUNCHES && DUAL_CRASH_BOTH >= 3 )); then
    ok "$DUAL_CRASH_LAUNCHES launches, $DUAL_CRASH_BOTH with both threads in the handler, a whole trace in every" \
      "one$DUAL_CRASH_DETAILS"
  elif (( DUAL_CRASH_WHOLE == DUAL_CRASH_LAUNCHES )); then
    unsure "a whole trace in all $DUAL_CRASH_LAUNCHES launches, but both threads reached the handler in only" \
      "$DUAL_CRASH_BOTH$DUAL_CRASH_DETAILS"
  else
    bad "$DUAL_CRASH_WHOLE of $DUAL_CRASH_LAUNCHES launches left a whole trace$DUAL_CRASH_DETAILS"
  fi
}

# Mutant SIMCAM_OFF_TRACE_CRASH_CLAIM=1 drops the CAS: both threads format into one buffer and write one temp file,
# 20 ms apart. trace-crash2's check must see a launch without a whole trace (torn, empty, unparseable, or a temp file
# left). A launch tears about half the time (6 of 13 in one run), so up to 10 launches miss it only about 0.1% of the
# time. A catch is a launch with both threads in the handler (2 crash lines) and no whole trace; see crash2_seq.
c_mutation-L70() {
  DUAL_CRASH_MUTANT=1 crash2_seq tc2m SIMCAM_OFF_TRACE_CRASH_CLAIM=1

  if (( DUAL_CRASH_CAUGHT )); then
    ok "mutant caught: launch $DUAL_CRASH_LAUNCHES of up to 10, both threads in the handler, left no whole trace" \
      "($DUAL_CRASH_BOTH with both threads)$DUAL_CRASH_DETAILS"
  elif (( DUAL_CRASH_BOTH == 0 )); then
    unsure "mutant not seen: the second thread never reached the handler in $DUAL_CRASH_LAUNCHES" \
      "launches$DUAL_CRASH_DETAILS"
  else
    bad "mutant not caught: every launch with both threads in the handler left a whole trace" \
      "($DUAL_CRASH_WHOLE of $DUAL_CRASH_LAUNCHES whole, $DUAL_CRASH_BOTH with both threads)$DUAL_CRASH_DETAILS"
  fi
}

# repick_seq <tag> <removed|unwritable> [K=V ...]: plain takes 2 photos and holds for 5 s. At its HOLD line the
# state dir is removed (or made mode 555). Then plain stops its session (a session-stop op) and exits. Sets these
# globals:
#   REPICK_PID            the launch's pid
#   REPICK_CLI_RC         the exit status of the CLI that read the trace
#   REPICK_SUMMARY        trace_summary of that trace
#   REPICK_LINE_COUNT     how many "trace: writing ... failed" (re-pick) lines the engine logged
#   REPICK_LINE           the first of them
#   REPICK_TRACE_BEFORE   "yes" if the pid's trace was in the state dir at HOLD, else "no"
repick_seq() {
  local tag=$1
  local how=$2
  shift 2
  clear_sentinel $PLAIN_BUNDLE
  bg_run plain $tag M5APP_PHOTOS=2 M5APP_HOLD=5 "$@"
  local dir=$BG_DIR
  wait_for $dir/app.log 'HOLD' 40
  REPICK_PID=$(launch_pid $dir/app.log)
  sleep 0.5
  REPICK_TRACE_BEFORE=$([[ -f $STATE_DIR/$PLAIN_BUNDLE/trace-$REPICK_PID.txt ]] && echo yes || echo no)

  # Mode 555 is undone on every path out of this block, by zsh's `always`. (An EXIT trap set in a function would fire
  # when the function returns, and an INT/TERM trap that doesn't exit would let the script go on after Ctrl-C.) Ctrl-C
  # and TERM exit the script, with 130 and 143, through the `always` block.
  {
    trap 'exit 130' INT
    trap 'exit 143' TERM
    if [[ $how == removed ]]; then
      rm -rf "${STATE_DIR:?}/${PLAIN_BUNDLE:?}"
    else
      chmod 555 "$STATE_DIR/$PLAIN_BUNDLE"
    fi
    wait $BG_PID
    sleep 1
  } always {
    # the app may have made the dir again in the meantime, or not
    [[ $how == unwritable ]] && chmod 755 "${STATE_DIR:?}/${PLAIN_BUNDLE:?}" 2>/dev/null
    trap - INT TERM
  }
  trace_json $REPICK_PID $dir/trace.json
  REPICK_CLI_RC=$?
  REPICK_SUMMARY=$(trace_summary $dir/trace.json)
  REPICK_LINE_COUNT=$(count_matches 'trace: writing .* failed' $dir/dylib.log)
  REPICK_LINE=$(grep -m1 -oE 'trace: writing .* failed.*' $dir/dylib.log)
  clear_sentinel $PLAIN_BUNDLE
}

# When the state dir is removed mid-run, the next write re-picks (it makes the dir again) and logs that once.
c_trace-repick() {
  repick_seq rp removed
  local -a fields
  fields=(${=REPICK_SUMMARY})

  if [[ $REPICK_TRACE_BEFORE == yes && $REPICK_CLI_RC == 0 && ${fields[1]} == state-dir &&
    $REPICK_SUMMARY == *session-stop* ]] &&
    [[ $REPICK_LINE_COUNT == 1 && $REPICK_LINE == *re-picked* ]]; then
    ok "pid $REPICK_PID: trace there before, dir removed at HOLD;" \
      "after exit ${fields[2]} ops incl. session-stop in the state dir again; 1 line: $REPICK_LINE"
  else
    bad "pid ${REPICK_PID:-?}: before=$REPICK_TRACE_BEFORE CLI rc=$REPICK_CLI_RC trace '${REPICK_SUMMARY:0:200}'" \
      "lines=$REPICK_LINE_COUNT '$REPICK_LINE'"
  fi
}

# When the state dir is made mode 555 mid-run, the trace is re-picked into the container-tmp fallback.
c_trace-unwritable() {
  repick_seq ru unwritable
  local -a fields
  fields=(${=REPICK_SUMMARY})

  if [[ $REPICK_TRACE_BEFORE == yes && $REPICK_CLI_RC == 0 && ${fields[1]} == container-tmp &&
    $REPICK_SUMMARY == *session-stop* ]] &&
    [[ $REPICK_LINE_COUNT == 1 && $REPICK_LINE == *container-tmp* ]]; then
    ok "pid $REPICK_PID: after exit the newest trace is the container-tmp one, ${fields[2]} ops incl. session-stop;" \
      "1 line: $REPICK_LINE"
  else
    bad "pid ${REPICK_PID:-?}: before=$REPICK_TRACE_BEFORE CLI rc=$REPICK_CLI_RC trace '${REPICK_SUMMARY:0:200}'" \
      "lines=$REPICK_LINE_COUNT '$REPICK_LINE'"
  fi
}

# Mutant SIMCAM_OFF_TRACE_REPICK=1 never re-picks: after the removal no trace is written any more (the dir stays
# gone).
c_mutation-L71() {
  repick_seq rpm removed SIMCAM_OFF_TRACE_REPICK=1

  if [[ $REPICK_TRACE_BEFORE == yes && $REPICK_CLI_RC != 0 && $REPICK_LINE_COUNT == 0 ]]; then
    ok "mutant caught: pid" \
      "$REPICK_PID's trace is gone after the removal and never written again (CLI rc=$REPICK_CLI_RC)"
  else
    bad "mutant not caught: before=$REPICK_TRACE_BEFORE CLI rc=$REPICK_CLI_RC trace '${REPICK_SUMMARY:0:160}'" \
      "lines=$REPICK_LINE_COUNT"
  fi
}

# flush_seq <tag> [K=V ...]: plain takes a photo, then calls stopRunning on its own thread with the photo callback
# 300 ms late. A start's flush is forced into the window between StopWaitBeginStop and the waiter's dispatch
# (SIMCAM_TEST_FLUSH_RACE). Sets FLUSH_RACE_LINE to the engine's verdict line.
flush_seq() {
  local tag=$1
  shift
  run plain $tag M5APP_PHOTOSTOP=bg SIMCAM_TEST_FINISH_DELAY_MS=300 SIMCAM_TEST_FLUSH_RACE=1 "$@"
  FLUSH_RACE_LINE=$(grep -m1 -oE 'SIMCAM_TEST_FLUSH_RACE: .*' "$(run_dir plain $tag)/dylib.log" | cut -c1-200)
}

# The flush returns only after the stop's DidStopRunning is queued.
c_flush-race() {
  flush_seq fr

  if [[ $FLUSH_RACE_LINE == *"outstanding=1"*"was queued first (in order)"* ]]; then
    ok "$FLUSH_RACE_LINE"
  else
    bad "${FLUSH_RACE_LINE:-no SIMCAM_TEST_FLUSH_RACE line}"
  fi
}

# Mutant SIMCAM_OFF_FLUSH_WAIT=1 brings back the unfixed dispatch_sync, which returns before the waiter is queued.
c_mutation-L73() {
  flush_seq frm SIMCAM_OFF_FLUSH_WAIT=1

  if [[ $FLUSH_RACE_LINE == *"NOT queued yet (out of order)"* ]]; then
    ok "mutant caught: $FLUSH_RACE_LINE"
  else
    bad "mutant not caught: ${FLUSH_RACE_LINE:-no SIMCAM_TEST_FLUSH_RACE line}"
  fi
}

# late_seq <tag> [K=V ...]: flush-race's flush, but the stop's waiter starts 650 ms late
# (SIMCAM_TEST_LATE_WAITER_MS), as if starved of CPU. The flush gives up at kStopWaitBoundS (500 ms) and the start
# would go ahead. The photo's didFinishCapture is 2 s late, so it is still due when the waiter runs. Sets these
# globals, together at its end:
#   LATE_FAULT_LINE     the SIMCAM_TEST_LATE_WAITER_MS line
#   LATE_TIMEOUT_LINE   the flush's timeout line
#   LATE_RACE_LINE      the SIMCAM_TEST_FLUSH_RACE line
#   LATE_WAITER_LINE    the waiter's verdict line
#   LATE_WAITER_MS      how long the waiter waited, in ms ("-" if its line doesn't say)
#   LATE_STOP_LINE      plain's stop line
late_seq() {
  local tag=$1
  shift
  run plain $tag M5APP_PHOTOSTOP=bg SIMCAM_TEST_FINISH_DELAY_MS=2000 SIMCAM_TEST_FLUSH_RACE=1 \
    SIMCAM_TEST_LATE_WAITER_MS=650 "$@"
  local dir=$(run_dir plain $tag)
  local engine_log=$dir/dylib.log

  # the waiter's verdict: flushed by a start, its photo callbacks done, or photos without didFinishCapture
  local waiter_rx='StopRunning: (flushed by a start|in-flight photo callbacks done|'
  waiter_rx+='[0-9]+ photo\(s\) without didFinishCapture) .*'

  # the flush's timeout line
  local timeout_rx='StartRunning: [0-9]+ deferred stop\(s\) still outstanding after .*'
  local fault_line=$(grep -m1 -oE 'SIMCAM_TEST_LATE_WAITER_MS: .*' $engine_log | cut -c1-120)
  local timeout_line=$(grep -m1 -oE "$timeout_rx" $engine_log | cut -c1-200)
  local race_line=$(grep -m1 -oE 'SIMCAM_TEST_FLUSH_RACE: .*' $engine_log | cut -c1-200)
  local waiter_line=$(grep -m1 -oE "$waiter_rx" $engine_log | cut -c1-160)
  local waiter_ms=$(print -r -- "$waiter_line" | grep -oE 'flushed by a start after [0-9.]+' | grep -oE '[0-9.]+$')
  local stop_line=$(grep -m1 -oE 'photostop.stop[^|]*stopRunning took [0-9]+ ms' $dir/app.log)

  LATE_FAULT_LINE=$fault_line
  LATE_TIMEOUT_LINE=$timeout_line
  LATE_RACE_LINE=$race_line
  LATE_WAITER_LINE=$waiter_line
  LATE_WAITER_MS=${waiter_ms:--}
  LATE_STOP_LINE=$stop_line
}

# The flush timed out with the mark kept, so the late waiter ends at once when it runs (under 50 ms) while its
# photo is still due.
c_late-waiter() {
  late_seq lw
  local late_waiter_max_ms=50

  if [[ -n $LATE_FAULT_LINE && $LATE_TIMEOUT_LINE == *"flush mark kept"* && $LATE_RACE_LINE == *"outstanding=1"* ]] &&
    [[ $LATE_WAITER_LINE == *"flushed by a start"*"still due"* && $LATE_WAITER_MS != - ]] &&
    (( LATE_WAITER_MS < late_waiter_max_ms )); then
    ok "$LATE_FAULT_LINE; flush: $LATE_TIMEOUT_LINE; the waiter: $LATE_WAITER_LINE; plain:" \
      "${LATE_STOP_LINE:-no stop line}"
  else
    bad "late=${LATE_FAULT_LINE:-none} | flush: ${LATE_TIMEOUT_LINE:-no timeout line} | race: ${LATE_RACE_LINE:-none}" \
      "| waiter: ${LATE_WAITER_LINE:-none} |" \
      "plain: ${LATE_STOP_LINE:-none}"
  fi
}

# Mutant SIMCAM_OFF_FLUSH_MARK_KEPT=1 clears the mark on timeout again, so the late waiter waits for its photo again.
c_mutation-L74() {
  late_seq lwm SIMCAM_OFF_FLUSH_MARK_KEPT=1

  if [[ $LATE_TIMEOUT_LINE == *"SIMCAM_OFF_FLUSH_MARK_KEPT"* && -n $LATE_WAITER_LINE &&
    $LATE_WAITER_LINE != *"flushed by a start"* ]]; then
    ok "mutant caught: the late waiter wasn't flushed: $LATE_WAITER_LINE; plain: ${LATE_STOP_LINE:-no stop line}"
  else
    bad "mutant not caught: flush: ${LATE_TIMEOUT_LINE:-no timeout line} | waiter: ${LATE_WAITER_LINE:-none}"
  fi
}
