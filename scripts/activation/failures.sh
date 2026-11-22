# SPDX-License-Identifier: Apache-2.0
# activation/failures.sh — activation that fails, throws or crashes, and the signal handlers around it.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

c_act-fail() {
  local events=$(events_log $PLAIN_BUNDLE)
  local failed=
  local summary=
  local events_seen dir mode want

  for mode in preflight throw missing pfbreak; do
    clear_sentinel $PLAIN_BUNDLE
    events_seen=$(line_count $events)
    if [[ $mode == pfbreak ]]; then
      run plain fl-$mode SIMCAM_PF_BREAK=1 M5APP_EXPECT=stock
      want='DISABLED - preflight failed'
    else
      run plain fl-$mode SIMCAM_TEST_ACTIVATE_FAIL=$mode M5APP_EXPECT=stock
      case $mode in
        preflight) want='DISABLED - preflight failed' ;;
        throw) want='activation threw SimcamTestActivateThrow' ;;
        missing) want='activation failed: .*not found' ;;
      esac
    fi
    dir=$(run_dir plain fl-$mode)
    # the engine's stderr lines in the app's log: all of them, and the one this failure wants
    local stderr_lines=$(grep -cE '^simcam: ' $dir/app.log)
    local wanted_lines=$(grep -cE "^simcam: .*$want" $dir/app.log)
    local markers=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N) $STATE_DIR/preflight-fail-$BUILD(N) \
      "$(fallback_state_dir $PLAIN_BUNDLE)"/latched-*(N))
    local crash_lines=$(lines_after $events $events_seen | grep -c 'crash activate .*exception=SimcamTestActivateThrow')
    local pass=$(pass_count $dir)
    local done_lines=$(count_matches DONE $dir/app.log)
    summary="$summary $mode:pass=$pass,stderr=$stderr_lines/$wanted_lines,markers=$markers,crash=$crash_lines"
    [[ $pass == 2 && $done_lines == 1 && $stderr_lines == 1 && $wanted_lines == 1 &&
      $markers == 0 ]] || failed="$failed $mode"
    [[ $mode == throw && $crash_lines != 1 ]] && failed="$failed throw-record"
    [[ $mode != throw && $crash_lines != 0 ]] && failed="$failed $mode-record"
  done

  if [[ -z $failed ]]; then
    ok "stock + one stderr line + DONE each, no marker; throw -> crash activate counted ($summary)"
  else
    bad "failed:$failed ($summary)"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# counted_strikes <events.log> <n>: the hang and crash lines after the first <n>, as "<kind> <feature>," each
counted_strikes() { lines_after $1 $2 | awk '$3=="hang"||$3=="crash"{print $3, $4}' | tr '\n' ','; }

# The first call times out, then prepare throws. That is exactly one counted strike, so the next launch arms.
c_act-timeout-throw() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen dir1 dir2 counted
  events_seen=$(line_count $events)
  run plain tt1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 SIMCAM_TEST_ACTIVATE_FAIL=throw M5APP_EXPECT=stock
  dir1=$(run_dir plain tt1)
  sleep 1
  counted=$(lines_after $events $events_seen | grep -cE '^[0-9]+ [0-9]+ (hang|crash) ')
  local strikes=$(counted_strikes $events $events_seen)
  run plain tt2
  dir2=$(run_dir plain tt2)
  local not_recorded=$(count_matches 'threw SimcamTestActivateThrow after the timeout; not recorded' $dir1/dylib.log)
  local stderr_lines=$(grep -cE '^simcam: ' $dir1/app.log)
  local armed2=$(count_matches 'LAZY armed by' $dir2/dylib.log)
  local pass2=$(pass_count $dir2)

  if [[ $counted == 1 && $strikes == "hang activate," && $not_recorded == 1 && $stderr_lines == 1 ]] &&
    [[ $armed2 == 1 && $pass2 == $PLAIN_CHECKS ]]; then
    ok "timeout at $(first_call_ms $dir1) ms, then prepare threw: 1 counted line ($strikes one stderr line)," \
      "'not recorded' logged; next launch armed 14/14"
  else
    bad "counted=$counted ($strikes) notrec=$(count_matches 'after the timeout; not recorded' $dir1/dylib.log)" \
      "stderr=$stderr_lines; next: armed=$armed2 pass=$pass2"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# The swapthrow failure throws in SWAP and again at the end of START. Each launch still counts only one strike.
c_act-swapthrow() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen counted1 counted2 swap_lines1 swap_lines2 dir1 dir2 dir3
  local counted_rx='^[0-9]+ [0-9]+ (hang|crash) '
  local swap_rx='^[0-9]+ [0-9]+ crash activate [^ ]+ exception=SimcamTestSwapThrow'
  events_seen=$(line_count $events)
  run plain swt1 SIMCAM_TEST_ACTIVATE_FAIL=swapthrow M5APP_EXPECT=stock
  counted1=$(lines_after $events $events_seen | grep -cE "$counted_rx")
  swap_lines1=$(lines_after $events $events_seen | grep -cE "$swap_rx")
  events_seen=$(line_count $events)
  dir1=$(run_dir plain swt1)
  run plain swt2 SIMCAM_TEST_ACTIVATE_FAIL=swapthrow M5APP_EXPECT=stock
  counted2=$(lines_after $events $events_seen | grep -cE "$counted_rx")
  swap_lines2=$(lines_after $events $events_seen | grep -cE "$swap_rx")
  dir2=$(run_dir plain swt2)
  run plain swt3 M5APP_EXPECT=stock
  dir3=$(run_dir plain swt3)
  local nr1=$(count_matches "START threw SimcamTestStartThrow; not recorded" $dir1/dylib.log)
  local nr2=$(count_matches "START threw SimcamTestStartThrow; not recorded" $dir2/dylib.log)
  local degraded=$(count_matches 'SWAP threw SimcamTestSwapThrow: armed but degraded' $dir1/dylib.log)
  local latched3=$(count_matches 'latched after' $dir3/dylib.log)
  local preflight3=$(count_matches PREFLIGHT $dir3/app.log)

  if [[ $counted1 == 1 && $swap_lines1 == 1 && $counted2 == 1 && $swap_lines2 == 1 && $nr1 == 1 && $nr2 == 1 &&
    $degraded == 1 ]] &&
    [[ $latched3 == 1 && $preflight3 == 0 ]]; then
    ok "1st/2nd: ARMED-degraded, SWAP and START both threw, exactly one counted line each" \
      "(crash activate exception=SimcamTestSwapThrow; START's 'not recorded'); 3rd latched in PREPARE 1"
  else
    bad "counted $counted1/$counted2 (swap lines $swap_lines1/$swap_lines2) start-not-recorded $nr1/$nr2" \
      "degraded=$(count_matches 'armed but degraded' $dir1/dylib.log) 3rd latched=$latched3"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# crash_seq <tag> <fail>: two launches that crash in activation (SIMCAM_TEST_ACTIVATE_FAIL=<fail>), then a normal
# one. Sets these globals, together at its end:
#   CRASH_RECORDS   how many 'crash activate' lines each crashing launch wrote: (<1st> <2nd>)
#   CRASH_FAULTS    ips_fault of each crash's report, or "no-report" if its pid has none: (<1st> <2nd>)
#   CRASH_PIDS      both pids, space-separated, from each launch's first [simcam <pid>] line (there is no fault line)
#   CRASH_DIR3      the normal launch's run dir
# Its crash_reports call also sets REPORTS_FOUND, PIDS_NO_REPORT and REPORT_FILE_OF; verdict_reports and report_of
# read them.
crash_seq() {
  local tag=$1
  local fail=$2
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen launch report
  local -a records faults pids
  clear_sentinel $PLAIN_BUNDLE

  for launch in 1 2; do
    events_seen=$(line_count $events)
    run plain $tag$launch SIMCAM_TEST_ACTIVATE_FAIL=$fail M5APP_EXPECT=stock
    records+=("$(lines_after $events $events_seen | grep -c 'crash activate')")
    pids+=($(fault_pid "$(run_dir plain $tag$launch)/app.log"))
  done

  crash_reports $PLAIN_PROCESS $CRASH_REPORT_WAIT_S $pids

  for launch in 1 2; do
    report=$(report_of ${pids[$launch]:-x})
    if [[ -n $report ]]; then
      faults+=("$(ips_fault $report || echo no-report)")   # a report ips_fault can't read counts as none
    else
      faults+=(no-report)
    fi
  done

  run plain ${tag}3 M5APP_EXPECT=stock

  CRASH_RECORDS=("${records[@]}")
  CRASH_FAULTS=("${faults[@]}")
  CRASH_PIDS="$pids"
  CRASH_DIR3=$(run_dir plain ${tag}3)
}

# report_matches <ips_fault text> <pattern>: the report-side check of act-crash-prepare and act-crash-fig. True if
# there is no report (verdict_reports makes a missing report UNSURE) or if the report matches the pattern.
report_matches() { [[ $1 == no-report ]] || [[ $1 == $~2 ]]; }

c_act-crash-prepare() {
  crash_seq cp crash
  local records="${CRASH_RECORDS[1]}/${CRASH_RECORDS[2]}"   # 1/1 is one record per crash
  local fault1=${CRASH_FAULTS[1]}
  local fault2=${CRASH_FAULTS[2]}
  local engine_ok=0
  local latched3=$(count_matches 'latched after' $CRASH_DIR3/dylib.log)

  if [[ $records == 1/1 && $latched3 == 1 ]] &&
    report_matches "$fault1" '*EXC_BAD_ACCESS*queue=simcam.activate*' &&
    report_matches "$fault2" '*queue=simcam.activate*'; then
    engine_ok=1
  fi

  local ok_text="one crash activate per crash (pids $CRASH_PIDS); reports: $fault1 / $fault2;"
  ok_text+=" 3rd latched"
  local bad_text="records $records; reports '$fault1' / '$fault2'"
  bad_text+=" (pids $CRASH_PIDS); 3rd latched=$latched3"
  verdict_reports $engine_ok 2 "$ok_text" "$bad_text"
  clear_sentinel $PLAIN_BUNDLE
}

c_act-crash-fig() {
  crash_seq cf crashfig
  local records="${CRASH_RECORDS[1]}/${CRASH_RECORDS[2]}"   # 1/1 is one record per crash
  local fault1=${CRASH_FAULTS[1]}
  local fault2=${CRASH_FAULTS[2]}
  local engine_ok=0
  local latched3=$(count_matches 'latched after' $CRASH_DIR3/dylib.log)

  if [[ $records == 1/1 && $latched3 == 1 && $fault1 != *simcam.activate* ]] &&
    report_matches "$fault1" '*top=CoreMedia*'; then
    engine_ok=1
  fi

  local ok_text="one crash activate per crash (pids $CRASH_PIDS; PC in CoreMedia on another thread:"
  ok_text+=" $fault1); 3rd latched"
  local bad_text="records $records; reports '$fault1' / '$fault2'"
  bad_text+=" (pids $CRASH_PIDS); 3rd latched=$latched3"
  verdict_reports $engine_ok 2 "$ok_text" "$bad_text"
  clear_sentinel $PLAIN_BUNDLE
}

# sigsegv_handler <run dir> [before|during]: the image of the SIGSEGV handler that the app's sigcheck saw after the
# first camera call, or before it, or during the prepare
sigsegv_handler() { grep -oE "sigcheck ${2:+$2 }SIGSEGV handler=[^ ]+" $1/app.log | cut -d= -f2; }

c_act-handler-restore() {
  local dir1 dir2 before1 after1 before2 after2 during2
  clear_sentinel $PLAIN_BUNDLE
  run plain hr1 SIMCAM_TEST_ACTIVATE_FAIL=preflight M5APP_SIGCHECK=1 M5APP_EXPECT=stock
  dir1=$(run_dir plain hr1)
  before1=$(sigsegv_handler $dir1 before)
  after1=$(sigsegv_handler $dir1)

  # Also read the handler while the never-finishing prepare runs (500 ms in). It must be ours (the minimal one);
  # otherwise the restore checks would pass even if it never went in.
  clear_sentinel $PLAIN_BUNDLE
  run plain hr2 SIMCAM_TEST_SLOW_ACTIVATE_MS=600000 M5APP_SIGCHECK=1 M5APP_SIGCHECK_AT_MS=500 M5APP_EXPECT=stock
  dir2=$(run_dir plain hr2)
  before2=$(sigsegv_handler $dir2 before)
  after2=$(sigsegv_handler $dir2)
  during2=$(sigsegv_handler $dir2 during)

  if [[ -n $before1 && $before1 == $after1 && -n $before2 && $before2 == $after2 &&
    $during2 == libsimcam.dylib* ]]; then
    ok "prepare failure: $before1 -> $after1; never-finishing prepare: $before2 -> during prepare $during2" \
      "-> after the waiter's restore $after2"
  else
    bad "preflight: '$before1' -> '$after1'; timeout: '$before2' -> during '$during2' -> '$after2'"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-fake-sdk() {
  # The fault comes at 1600 ms. With M5APP_EXPECT=stock, the app exits about 1 s after the first call returns.
  # The fault is in a related image: CoreMedia, on a global queue (SIMCAM_TEST_FIG_FAULT_AT_MS).
  # The minimal handler would record it as `crash activate` if gActRunning were still set. The waiter clears that flag
  # before anything else. So the chain from the fake SDK's handler to our minimal handler must record nothing.
  # A fault in app code is never recorded either way.
  clear_sentinel $PLAIN_BUNDLE
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen dir
  events_seen=$(line_count $events)
  run plain fk SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_FAKESDK_AT_MS=500 M5APP_SIGCHECK=1 \
    SIMCAM_TEST_FIG_FAULT_AT_MS=1600 M5APP_EXPECT=stock
  dir=$(run_dir plain fk)
  sleep 2
  local after=$(sigsegv_handler $dir)
  local sdk_records=$(count_matches 'FAKESDK record' $dir/app.log)
  local our_lines=$(lines_after $events $events_seen | grep -cE 'crash activate|crash-other')
  local pid=$(fault_pid $dir/app.log 'SIMCAM_TEST_FIG_FAULT_AT_MS: CoreMedia fault now')
  local fault=
  local engine_ok=0
  local report
  crash_reports $PLAIN_PROCESS $CRASH_REPORT_WAIT_S $pid
  report=$(report_of ${pid:-x})
  [[ -n $report ]] && fault=$(ips_fault $report)

  if [[ $after == $PLAIN_PROCESS* && $sdk_records == 1 && $our_lines == 0 && -n $pid &&
    ( -z $report || $fault == *top=CoreMedia* ) ]]; then
    engine_ok=1
  fi

  local ok_text="after the timeout the SDK's handler is current ($after);"
  ok_text+=" the CoreMedia fault at 1600 ms (pid $pid, report: ${fault:-none}) gave one FAKESDK record,"
  ok_text+=" no crash activate/crash-other line"
  verdict_reports $engine_ok 1 "$ok_text" \
    "after=$after sdkRecords=$sdk_records ourLines=$our_lines fault pid=${pid:-none} report='$fault'"
  clear_sentinel $PLAIN_BUNDLE
}

c_act-armed-crash() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen
  events_seen=$(line_count $events)
  run plain ac SIMCAM_WD_TEST=crash
  sleep 1
  local photo_crashes=$(lines_after $events $events_seen | grep -c 'crash photo')
  local activate_crashes=$(lines_after $events $events_seen | grep -c 'crash activate')

  if [[ $photo_crashes == 1 && $activate_crashes == 0 ]]; then
    ok "exactly one crash photo line (full handler only), no crash activate"
  else
    bad "crash photo=$photo_crashes crash activate=$activate_crashes"
  fi

  clear_sentinel $PLAIN_BUNDLE
}
