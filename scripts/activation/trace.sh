# SPDX-License-Identifier: Apache-2.0
# activation/trace.sh — the op trace file and a start's flush of a deferred stop.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
# ---------------------------------------------------------------- op trace file, deferred-stop flush
# The op trace's file (core/trace.inc; ledger L69-L71) and a start's flush of a deferred stop (fig/session_state.inc; L73).
# Traces are read with the CLI (`stuntcam trace --json --pid`), the parser `stuntcam trace` users get.
SCLI=$ROOT/cli/bin/stuntcam
trj() { $SCLI trace --json --udid $U --pid $1 $BP > $2 2>$2.err; }   # trj <pid> <out.json>: rc of the CLI
trsum() {   # trsum <json>: "<location> <n ops> <n crash ops> <ms from activate to the first crash op | -> <ops joined by ,>"
  python3 -I -c '
import json, sys
r = json.load(open(sys.argv[1])); o = r["ops"]; n = [x["op"] for x in o]
a = [x for x in o if x["op"] == "activate"]; c = [x for x in o if x["op"] == "crash"]
ms = "%.1f" % ((c[0]["wall_ns"] - a[0]["wall_ns"]) / 1e6) if a and c else "-"
print(r["location"], len(n), n.count("crash"), ms, ",".join(n))' $1 2>/dev/null || echo "unparseable"
}
lpid() { grep -m1 -oE '\[simcam [0-9]+\]' $1 2>/dev/null | grep -oE '[0-9]+'; }   # lpid <app.log>: the launch's pid
# trace_crash <tag> <SIMCAM_TEST_ACTIVATE_FAIL value> [K=V ...]: one plain launch that faults in START; sets TC_PID, TC_RC (CLI),
# TC_SUM (trsum), TC_EV (crash lines in events.log since), TC_TMP (temp files of that pid left)
trace_crash() {
  local tag=$1 f=$2; shift 2
  clear_sentinel $BP; local e=$(ev $BP) n0; n0=$(nl $e)
  run plain $tag SIMCAM_TEST_ACTIVATE_FAIL=$f "$@"; sleep 1; local d=$(res plain $tag)
  TC_PID=$(fault_pid $d/app.log 'SIMCAM_TEST_ACTIVATE_FAIL=') TC_EV=$(since $e $n0 | grep -c ' crash')
  [[ -n $TC_PID ]] || { TC_RC=x TC_SUM="no fault line in $d/app.log" TC_TMP=0; clear_sentinel $BP; return; }
  trj $TC_PID $d/trace.json; TC_RC=$?; TC_SUM=$(trsum $d/trace.json)
  TC_TMP=$(nf $STATE/$BP/.trace-$TC_PID.*(N) $(tmpdir $BP)/.trace-$TC_PID.*(N))
  clear_sentinel $BP
}
c_trace-crash-start() {   # L69: a crash in START (before the first 150 ms flush) leaves the trace, from the handler
  trace_crash tcs crashstart
  local -a w; w=(${=TC_SUM})
  [[ $TC_RC == 0 && ${w[3]} == 1 && $TC_SUM == *activate*crash* && $TC_EV == 1 && $TC_TMP == 0 ]] &&
    ok "pid $TC_PID: trace written by the crash handler, parsed by the CLI: ${w[1]}, ${w[2]} ops, crash ${w[4]} ms after activate (${w[5]}); 1 crash line" \
    ||
      bad "pid ${TC_PID:-?}: CLI rc=$TC_RC trace '$TC_SUM' crash lines=$TC_EV temp files left=$TC_TMP ($(head -c 200 "$(res plain tcs)/trace.json.err" 2>/dev/null))"
}
c_mutation-L69() {   # SIMCAM_OFF_TRACE_DIR_AT_ARM=1: the directory picked at the first flush again: the same crash leaves no trace
  trace_crash tcl crashstart SIMCAM_OFF_TRACE_DIR_AT_ARM=1
  [[ $TC_RC != 0 && $TC_EV == 1 ]] &&
    ok "mutant caught: pid $TC_PID crashed (1 crash line) and left no trace (CLI rc=$TC_RC: $(head -c 120 "$(res plain tcl)/trace.json.err" 2>/dev/null))" \
    || bad "mutant not caught: pid ${TC_PID:-?} CLI rc=$TC_RC trace '$TC_SUM' crash lines=$TC_EV"
}
# crash2_seq <tag> [K=V ...]: up to 6 crash2 launches, until 3 had both threads in the handler (2 crash lines; under heavy
# load the second thread may not run before the process ends: such a launch counts only for the whole-trace check). With
# C2_STOP_BAD=1 (the mutant): up to 10 launches, until one with both threads in the handler leaves no whole trace (C2_CAUGHT:
# the fault line was found, so the CLI ran, and 2 crash lines; a launch without them that isn't whole is no catch, it can't
# have raced). Sets C2_N (launches), C2_BOTH (with both threads in the handler), C2_WHOLE (with a whole trace), C2_CAUGHT
# and C2_DET (each launch's details, for the verdict line)
crash2_seq() {
  local tag=$1 i mut=${C2_STOP_BAD:-0}; shift; C2_N=0 C2_BOTH=0 C2_WHOLE=0 C2_CAUGHT=0 C2_DET=
  for i in {1..$(( mut ? 10 : 6 ))}; do
    (( ! mut && C2_BOTH >= 3 )) && break
    trace_crash $tag-$i crash2 "$@"; C2_N=$((C2_N + 1))
    local -a w; w=(${=TC_SUM})
    local good=0; [[ $TC_RC == 0 && ${w[3]:-0} -ge 1 && $TC_TMP == 0 ]] && good=1 C2_WHOLE=$((C2_WHOLE + 1))
    [[ $TC_EV == 2 ]] && C2_BOTH=$((C2_BOTH + 1))
    C2_DET="$C2_DET [pid $TC_PID rc=$TC_RC crash lines=$TC_EV crash ops=${w[3]:-?} ops=${w[2]:-?} tmp=$TC_TMP]"
    (( mut && ! good )) && [[ $TC_RC != x && $TC_EV == 2 ]] && { C2_CAUGHT=1; break; }
  done
}
c_trace-crash2() {   # L70: two threads in the crash handler at once: one whole trace (parsed by the CLI). Needs 3 launches in
  # which both threads ran the handler, at most 6 launches; any launch without a whole trace FAILs
  crash2_seq tc2
  if (( C2_WHOLE == C2_N && C2_BOTH >= 3 )); then ok "$C2_N launches, $C2_BOTH with both threads in the handler, a whole trace in every one$C2_DET"
  elif (( C2_WHOLE == C2_N )); then unsure "a whole trace in all $C2_N launches, but both threads reached the handler in only $C2_BOTH$C2_DET"
  else bad "$C2_WHOLE of $C2_N launches left a whole trace$C2_DET"; fi
}
c_mutation-L70() {   # SIMCAM_OFF_TRACE_CRASH_CLAIM=1: no CAS, both threads format into one buffer and write one temp file, 20 ms apart:
  # trace-crash2's check must see a launch without a whole trace (torn, empty, unparseable or a temp file left). A launch
  # tears about half the time (6 of 13 in one run), so up to 10 launches (a miss ~0.1%)
  # A catch is a launch with both threads in the handler (2 crash lines) and no whole trace; see crash2_seq
  C2_STOP_BAD=1 crash2_seq tc2m SIMCAM_OFF_TRACE_CRASH_CLAIM=1
  if (( C2_CAUGHT ))
  then ok "mutant caught: launch $C2_N of up to 10, both threads in the handler, left no whole trace ($C2_BOTH with both threads)$C2_DET"
  elif (( C2_BOTH == 0 )); then unsure "mutant not seen: the second thread never reached the handler in $C2_N launches$C2_DET"
  else bad "mutant not caught: every launch with both threads in the handler left a whole trace ($C2_WHOLE of $C2_N whole, $C2_BOTH with both threads)$C2_DET"
  fi
}
# repick_seq <tag> <removed|unwritable> [K=V ...]: plain takes 2 photos, holds 5 s; at HOLD the state dir is removed (or made
# mode 555); then it stops (session-stop op) and exits. Sets RP_PID, RP_RC (the CLI's), RP_SUM (trsum), RP_LINES (re-pick
# log lines) and RP_LINE (the first), RP_BEFORE (yes if the pid's trace was in the state dir at HOLD)
repick_seq() {
  local tag=$1 how=$2; shift 2; clear_sentinel $BP
  bg_run plain $tag M5APP_PHOTOS=2 M5APP_HOLD=5 "$@"; local d=$BGDIR
  wait_for $d/app.log 'HOLD' 40; RP_PID=$(lpid $d/app.log); sleep 0.5
  RP_BEFORE=$([[ -f $STATE/$BP/trace-$RP_PID.txt ]] && echo yes || echo no)
  # mode 555 is undone on every path out of this block (zsh `always`; an EXIT trap set in a function would fire at its return,
  # and an INT/TERM trap that doesn't exit would let Ctrl-C go on): Ctrl-C / TERM exit the script (130 / 143) through it
  {
    trap 'exit 130' INT; trap 'exit 143' TERM
    if [[ $how == removed ]]; then rm -rf "${STATE:?}/${BP:?}"; else chmod 555 "$STATE/$BP"; fi
    wait $BGPID; sleep 1
  } always { [[ $how == unwritable ]] && chmod 755 "${STATE:?}/${BP:?}" 2>/dev/null; trap - INT TERM; }
  trj $RP_PID $d/trace.json; RP_RC=$?; RP_SUM=$(trsum $d/trace.json)
  RP_LINES=$(cnt 'trace: writing .* failed' $d/dylib.log); RP_LINE=$(grep -m1 -oE 'trace: writing .* failed.*' $d/dylib.log)
  clear_sentinel $BP
}
c_trace-repick() {   # L71: the state dir removed mid-run: the next write re-picks (the dir is made again) and logs it once
  repick_seq rp removed
  local -a w; w=(${=RP_SUM})
  [[ $RP_BEFORE == yes && $RP_RC == 0 && ${w[1]} == state-dir && $RP_SUM == *session-stop* && $RP_LINES == 1 && $RP_LINE == *re-picked* ]] \
    && ok "pid $RP_PID: trace there before, dir removed at HOLD; after exit ${w[2]} ops incl. session-stop in the state dir again; 1 line: $RP_LINE" \
    || bad "pid ${RP_PID:-?}: before=$RP_BEFORE CLI rc=$RP_RC trace '${RP_SUM:0:200}' lines=$RP_LINES '$RP_LINE'"
}
c_trace-unwritable() {   # L71: the state dir made mode 555 mid-run: re-picked into the container-tmp fallback
  repick_seq ru unwritable
  local -a w; w=(${=RP_SUM})
  [[ $RP_BEFORE == yes && $RP_RC == 0 && ${w[1]} == container-tmp && $RP_SUM == *session-stop* && $RP_LINES == 1 && $RP_LINE == *container-tmp* ]] \
    && ok "pid $RP_PID: after exit the newest trace is the container-tmp one, ${w[2]} ops incl. session-stop; 1 line: $RP_LINE" \
    || bad "pid ${RP_PID:-?}: before=$RP_BEFORE CLI rc=$RP_RC trace '${RP_SUM:0:200}' lines=$RP_LINES '$RP_LINE'"
}
c_mutation-L71() {   # SIMCAM_OFF_TRACE_REPICK=1: no re-pick: after the removal no trace is written any more (the dir stays gone)
  repick_seq rpm removed SIMCAM_OFF_TRACE_REPICK=1
  [[ $RP_BEFORE == yes && $RP_RC != 0 && $RP_LINES == 0 ]] &&
    ok "mutant caught: pid $RP_PID's trace is gone after the removal and never written again (CLI rc=$RP_RC)" \
    || bad "mutant not caught: before=$RP_BEFORE CLI rc=$RP_RC trace '${RP_SUM:0:160}' lines=$RP_LINES"
}
# flush_seq <tag> [K=V ...]: a photo, then stopRunning on plain's thread with the callback 300 ms late, and a start's flush
# forced into the window between StopWaitBeginStop and the waiter's dispatch (SIMCAM_TEST_FLUSH_RACE); FR_LINE = the engine's verdict line
flush_seq() { local tag=$1; shift; run plain $tag M5APP_PHOTOSTOP=bg SIMCAM_TEST_FINISH_DELAY_MS=300 SIMCAM_TEST_FLUSH_RACE=1 "$@"; FR_LINE=$(grep -m1 -oE 'SIMCAM_TEST_FLUSH_RACE: .*' "$(res plain $tag)/dylib.log" | cut -c1-200); }
c_flush-race() {   # L73: the flush returns only after the stop's DidStopRunning is queued
  flush_seq fr
  [[ $FR_LINE == *"outstanding=1"*"was queued first (in order)"* ]] && ok "$FR_LINE" || bad "${FR_LINE:-no SIMCAM_TEST_FLUSH_RACE line}"
}
c_mutation-L73() {   # SIMCAM_OFF_FLUSH_WAIT=1: the unfixed dispatch_sync returns before the waiter is queued
  flush_seq frm SIMCAM_OFF_FLUSH_WAIT=1
  [[ $FR_LINE == *"NOT queued yet (out of order)"* ]] && ok "mutant caught: $FR_LINE" || bad "mutant not caught: ${FR_LINE:-no SIMCAM_TEST_FLUSH_RACE line}"
}
# late_seq <tag> [K=V ...] (L74): flush-race's flush, but the stop's waiter starts 650 ms late (SIMCAM_TEST_LATE_WAITER_MS), as if
# starved of CPU: the flush gives up at kStopWaitBoundS (500 ms) and the start would go ahead; the photo's didFinishCapture is
# 2 s late, so it is still due when the waiter runs. Sets LW_LATE (the fault's line), LW_TO (the flush's timeout line),
# LW_FR (the SIMCAM_TEST_FLUSH_RACE line), LW_W (the waiter's verdict line), LW_MS (its wait in ms, or -), LW_STOP (plain's stop line)
late_seq() {
  local tag=$1; shift
  run plain $tag M5APP_PHOTOSTOP=bg SIMCAM_TEST_FINISH_DELAY_MS=2000 SIMCAM_TEST_FLUSH_RACE=1 SIMCAM_TEST_LATE_WAITER_MS=650 "$@"; local d=$(res plain $tag)
  LW_LATE=$(grep -m1 -oE 'SIMCAM_TEST_LATE_WAITER_MS: .*' $d/dylib.log | cut -c1-120)
  LW_TO=$(grep -m1 -oE 'StartRunning: [0-9]+ deferred stop\(s\) still outstanding after .*' $d/dylib.log | cut -c1-200)
  LW_FR=$(grep -m1 -oE 'SIMCAM_TEST_FLUSH_RACE: .*' $d/dylib.log | cut -c1-200)
  LW_W=$(grep -m1 -oE 'StopRunning: (flushed by a start|in-flight photo callbacks done|[0-9]+ photo\(s\) without didFinishCapture) .*' $d/dylib.log | cut -c1-160)
  LW_MS=$(print -r -- "$LW_W" | grep -oE 'flushed by a start after [0-9.]+' | grep -oE '[0-9.]+$'); LW_MS=${LW_MS:--}
  LW_STOP=$(grep -m1 -oE 'photostop.stop[^|]*stopRunning took [0-9]+ ms' $d/app.log)
}
c_late-waiter() {   # L74: the flush timed out with the mark kept: the late waiter ends at once when it runs (< 50 ms), its photo still due
  late_seq lw
  if [[ -n $LW_LATE && $LW_TO == *"flush mark kept"* && $LW_FR == *"outstanding=1"* &&
    $LW_W == *"flushed by a start"*"still due"* && $LW_MS != - ]] && (( LW_MS < 50 )); then
    ok "$LW_LATE; flush: $LW_TO; the waiter: $LW_W; plain: ${LW_STOP:-no stop line}"
  else bad "late=${LW_LATE:-none} | flush: ${LW_TO:-no timeout line} | race: ${LW_FR:-none} | waiter: ${LW_W:-none} | plain: ${LW_STOP:-none}"; fi
}
c_mutation-L74() {   # SIMCAM_OFF_FLUSH_MARK_KEPT=1: the mark cleared on timeout again: the late waiter waits for its photo again
  late_seq lwm SIMCAM_OFF_FLUSH_MARK_KEPT=1
  if [[ $LW_TO == *"SIMCAM_OFF_FLUSH_MARK_KEPT"* && -n $LW_W && $LW_W != *"flushed by a start"* ]]
  then ok "mutant caught: the late waiter wasn't flushed: $LW_W; plain: ${LW_STOP:-no stop line}"
  else bad "mutant not caught: flush: ${LW_TO:-no timeout line} | waiter: ${LW_W:-none}"; fi
}
