# SPDX-License-Identifier: Apache-2.0
# activation/failures.sh — activation that fails, throws or crashes, and the signal handlers around it.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
c_act-fail() {
  local e=$(ev $BP) n0 d f out= sum= want
  for f in preflight throw missing pfbreak; do
    clear_sentinel $BP; n0=$(nl $e)
    if [[ $f == pfbreak ]]; then run plain fl-$f SIMCAM_PF_BREAK=1 M5APP_EXPECT=stock; want='DISABLED - preflight failed'
    else run plain fl-$f SIMCAM_TEST_ACTIVATE_FAIL=$f M5APP_EXPECT=stock; case $f in preflight) want='DISABLED - preflight failed';; throw) want='activation threw SimcamTestActivateThrow';; missing) want='activation failed: .*not found';; esac; fi
    d=$(res plain fl-$f)
    local raw=$(grep -cE '^simcam: ' $d/app.log) w=$(grep -cE "^simcam: .*$want" $d/app.log)
    local mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N) "$(tmpdir $BP)"/latched-*(N)) ca=$(since $e $n0 | grep -c 'crash activate .*exception=SimcamTestActivateThrow')
    sum="$sum $f:pass=$(pass_n $d),stderr=$raw/$w,markers=$mk,crash=$ca"
    [[ $(pass_n $d) == 2 && $(cnt DONE $d/app.log) == 1 && $raw == 1 && $w == 1 && $mk == 0 ]] || out="$out $f"
    [[ $f == throw && $ca != 1 ]] && out="$out throw-record"
    [[ $f != throw && $ca != 0 ]] && out="$out $f-record"
  done
  [[ -z $out ]] && ok "stock + one stderr line + DONE each, no marker; throw -> crash activate counted ($sum)" || bad "failed:$out ($sum)"
  clear_sentinel $BP
}
c_act-timeout-throw() {   # a timeout, then prepare throws: exactly one counted strike, so the next launch arms
  clear_sentinel $BP; local e=$(ev $BP) n0 d1 d2 c; n0=$(nl $e)
  run plain tt1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 SIMCAM_TEST_ACTIVATE_FAIL=throw M5APP_EXPECT=stock; d1=$(res plain tt1); sleep 1
  c=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ (hang|crash) ')
  local lines=$(since $e $n0 | awk '$3=="hang"||$3=="crash"{print $3, $4}' | tr '\n' ',')
  run plain tt2; d2=$(res plain tt2)
  if [[ $c == 1 && $lines == "hang activate," && $(cnt 'threw SimcamTestActivateThrow after the timeout; not recorded' $d1/dylib.log) == 1 && $(grep -cE '^simcam: ' $d1/app.log) == 1 &&
        $(cnt 'LAZY armed by' $d2/dylib.log) == 1 && $(pass_n $d2) == 14 ]]
  then ok "timeout at $(fc $d1) ms, then prepare threw: 1 counted line ($lines one stderr line), 'not recorded' logged; next launch armed 14/14"
  else bad "counted=$c ($lines) notrec=$(cnt 'after the timeout; not recorded' $d1/dylib.log) stderr=$(grep -cE '^simcam: ' $d1/app.log); next: armed=$(cnt 'LAZY armed by' $d2/dylib.log) pass=$(pass_n $d2)"
  fi
  clear_sentinel $BP
}
c_act-swapthrow() {   # swapthrow throws in SWAP and again at START's end: one counted strike per launch all the same
  clear_sentinel $BP; local e=$(ev $BP) n0 c1 c2 l1 l2 d1 d2 d3; n0=$(nl $e)
  run plain swt1 SIMCAM_TEST_ACTIVATE_FAIL=swapthrow M5APP_EXPECT=stock
  c1=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ (hang|crash) ')
  l1=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ crash activate [^ ]+ exception=SimcamTestSwapThrow'); n0=$(nl $e)
  d1=$(res plain swt1)
  run plain swt2 SIMCAM_TEST_ACTIVATE_FAIL=swapthrow M5APP_EXPECT=stock
  c2=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ (hang|crash) ')
  l2=$(since $e $n0 | grep -cE '^[0-9]+ [0-9]+ crash activate [^ ]+ exception=SimcamTestSwapThrow'); d2=$(res plain swt2)
  run plain swt3 M5APP_EXPECT=stock; d3=$(res plain swt3)
  local nr1=$(cnt "START threw SimcamTestStartThrow; not recorded" $d1/dylib.log) nr2=$(cnt "START threw SimcamTestStartThrow; not recorded" $d2/dylib.log)
  if [[ $c1 == 1 && $l1 == 1 && $c2 == 1 && $l2 == 1 && $nr1 == 1 && $nr2 == 1 && $(cnt 'SWAP threw SimcamTestSwapThrow: armed but degraded' $d1/dylib.log) == 1 &&
        $(cnt 'latched after' $d3/dylib.log) == 1 && $(cnt PREFLIGHT $d3/app.log) == 0 ]]
  then ok "1st/2nd: ARMED-degraded, SWAP and START both threw, exactly one counted line each (crash activate exception=SimcamTestSwapThrow; START's 'not recorded'); 3rd latched in PREPARE 1"
  else bad "counted $c1/$c2 (swap lines $l1/$l2) start-not-recorded $nr1/$nr2 degraded=$(cnt 'armed but degraded' $d1/dylib.log) 3rd latched=$(cnt 'latched after' $d3/dylib.log)"
  fi
  clear_sentinel $BP
}
# crash_seq <tag> <fail>: two crashing launches, then a normal one. Sets C1 C2 (each crash's count of
# 'crash activate' lines), F1 F2 (ips_fault of each crash's report, "no-report" if the pid has none), CS_PIDS (both
# pids: the launch's first [simcam <pid>] line, there is no fault line here), D3 (the normal launch's run dir), and CR/CR_NO (crash_reports)
crash_seq() {
  local t=$1 f=$2 e=$(ev $BP) n0 rp; local -a pids
  clear_sentinel $BP
  for i in 1 2; do
    n0=$(nl $e); run plain $t$i SIMCAM_TEST_ACTIVATE_FAIL=$f M5APP_EXPECT=stock
    eval "C$i=\$(since \$e \$n0 | grep -c 'crash activate')"; pids+=($(fault_pid "$(res plain $t$i)/app.log"))
  done
  crash_reports m5app 30 $pids
  for i in 1 2; do rp=$(report_of ${pids[$i]:-x}); eval "F$i=\"\$( [[ -n \$rp ]] && ips_fault \$rp || echo no-report )\""; done
  CS_PIDS="$pids"
  run plain ${t}3 M5APP_EXPECT=stock; D3=$(res plain ${t}3)
}
# the report-side checks of act-crash-prepare / act-crash-fig run only on the reports that exist (a missing one is UNSURE)
rep_ok() { [[ $1 == no-report ]] || [[ $1 == $~2 ]]; }
c_act-crash-prepare() {
  crash_seq cp crash
  local eng=0; [[ $C1 == 1 && $C2 == 1 && $(cnt 'latched after' $D3/dylib.log) == 1 ]] &&
    rep_ok "$F1" '*EXC_BAD_ACCESS*queue=simcam.activate*' && rep_ok "$F2" '*queue=simcam.activate*' && eng=1
  verdict_reports $eng 2 "one crash activate per crash (pids $CS_PIDS); reports: $F1 / $F2; 3rd latched" "records $C1/$C2; reports '$F1' / '$F2' (pids $CS_PIDS); 3rd latched=$(cnt 'latched after' $D3/dylib.log)"
  clear_sentinel $BP
}
c_act-crash-fig() {
  crash_seq cf crashfig
  local eng=0; [[ $C1 == 1 && $C2 == 1 && $(cnt 'latched after' $D3/dylib.log) == 1 && $F1 != *simcam.activate* ]] &&
    rep_ok "$F1" '*top=CoreMedia*' && eng=1
  verdict_reports $eng 2 "one crash activate per crash (pids $CS_PIDS; PC in CoreMedia on another thread: $F1); 3rd latched" "records $C1/$C2; reports '$F1' / '$F2' (pids $CS_PIDS); 3rd latched=$(cnt 'latched after' $D3/dylib.log)"
  clear_sentinel $BP
}
c_act-handler-restore() {
  local d1 d2 b1 a1 b2 a2 m2
  clear_sentinel $BP; run plain hr1 SIMCAM_TEST_ACTIVATE_FAIL=preflight M5APP_SIGCHECK=1 M5APP_EXPECT=stock; d1=$(res plain hr1)
  b1=$(grep -oE 'sigcheck before SIGSEGV handler=[^ ]+' $d1/app.log | cut -d= -f2)
  a1=$(grep -oE 'sigcheck SIGSEGV handler=[^ ]+' $d1/app.log | cut -d= -f2)
  # also read the handler while the never-finishing prepare runs (500 ms in): it must be ours (the minimal one),
  # or the restore checks would pass without it ever going in
  clear_sentinel $BP; run plain hr2 SIMCAM_TEST_SLOW_ACTIVATE_MS=600000 M5APP_SIGCHECK=1 M5APP_SIGCHECK_AT_MS=500 M5APP_EXPECT=stock; d2=$(res plain hr2)
  b2=$(grep -oE 'sigcheck before SIGSEGV handler=[^ ]+' $d2/app.log | cut -d= -f2)
  a2=$(grep -oE 'sigcheck SIGSEGV handler=[^ ]+' $d2/app.log | cut -d= -f2)
  m2=$(grep -oE 'sigcheck during SIGSEGV handler=[^ ]+' $d2/app.log | cut -d= -f2)
  if [[ -n $b1 && $b1 == $a1 && -n $b2 && $b2 == $a2 && $m2 == libsimcam.dylib* ]]
  then ok "prepare failure: $b1 -> $a1; never-finishing prepare: $b2 -> during prepare $m2 -> after the waiter's restore $a2"
  else bad "preflight: '$b1' -> '$a1'; timeout: '$b2' -> during '$m2' -> '$a2'"; fi
  clear_sentinel $BP
}
c_act-fake-sdk() {
  # the fault comes at 1600 ms: M5APP_EXPECT=stock exits ~1 s after the first call returns. It is a
  # related-image fault (CoreMedia, on a global queue: SIMCAM_TEST_FIG_FAULT_AT_MS), which the minimal handler would record
  # as `crash activate` if gActRunning were still set: the waiter clears it before anything else, so the chain through the
  # fake SDK to our minimal handler must record nothing. (An app-code fault is never recorded either way.)
  clear_sentinel $BP; local e=$(ev $BP) n0 d; n0=$(nl $e)
  run plain fk SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_FAKESDK_AT_MS=500 M5APP_SIGCHECK=1 SIMCAM_TEST_FIG_FAULT_AT_MS=1600 M5APP_EXPECT=stock
  d=$(res plain fk); sleep 2
  local after=$(grep -oE 'sigcheck SIGSEGV handler=[^ ]+' $d/app.log | cut -d= -f2) rec=$(cnt 'FAKESDK record' $d/app.log) ours=$(since $e $n0 | grep -cE 'crash activate|crash-other')
  local pid=$(fault_pid $d/app.log 'SIMCAM_TEST_FIG_FAULT_AT_MS: CoreMedia fault now') rp fl= eng=0; crash_reports m5app 30 $pid
  rp=$(report_of ${pid:-x}); [[ -n $rp ]] && fl=$(ips_fault $rp)
  [[ $after == m5app* && $rec == 1 && $ours == 0 && -n $pid && ( -z $rp || $fl == *top=CoreMedia* ) ]] && eng=1
  verdict_reports $eng 1 "after the timeout the SDK's handler is current ($after); the CoreMedia fault at 1600 ms (pid $pid, report: ${fl:-none}) gave one FAKESDK record, no crash activate/crash-other line" \
    "after=$after sdkRecords=$rec ourLines=$ours fault pid=${pid:-none} report='$fl'"
  clear_sentinel $BP
}
c_act-armed-crash() {
  clear_sentinel $BP; local e=$(ev $BP) n0; n0=$(nl $e)
  run plain ac SIMCAM_WD_TEST=crash; sleep 1
  local c=$(since $e $n0 | grep -c 'crash photo') a=$(since $e $n0 | grep -c 'crash activate')
  [[ $c == 1 && $a == 0 ]] && ok "exactly one crash photo line (full handler only), no crash activate" || bad "crash photo=$c crash activate=$a"
  clear_sentinel $BP
}
