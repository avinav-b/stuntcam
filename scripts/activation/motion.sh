# SPDX-License-Identifier: Apache-2.0
# activation/motion.sh — the CoreMotion stand-in: at load, decided from markers, its crashes, the attitude API.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# the stock line's text in the engine log: "CoreMotion is stock this launch (<reason>)"
STOCK_LINE_RX='CoreMotion is stock this launch \([^)]*\)'

c_motion-only() {
  local launches=${MOTION_N:-10}
  local passed=0
  local failures=
  local i dir

  for i in {1..$launches}; do
    run plain mo$i M5APP_ORDER=motion-only
    dir=$(run_dir plain mo$i)
    # line numbers of the app's first query and of the tracker's start: the tracker must start after the query
    local order=$(first_line_number 'M5APP\| order=motion-only' $dir/app.log)
    local tracking=$(first_line_number 'tracking started by' $dir/app.log)
    local pass=$(count_matches 'PASS order.motion-only' $dir/app.log)
    local armed=$(count_matches 'LAZY armed' $dir/app.log)
    local done_lines=$(count_matches DONE $dir/app.log)
    if [[ $pass == 3 && $armed == 0 && -n $tracking && -n $order ]] && (( tracking > order )) &&
      [[ $done_lines == 1 ]]; then
      passed=$((passed + 1))
    else
      failures="$failures [$i: pass=$pass armed=$armed tracking@$tracking order@$order]"
    fi
  done

  if (( passed == launches )); then
    ok "$launches/$launches: stand-in answers (available, rate, timestamps), never armed," \
      "'$(grep -m1 -oE 'tracking started by [^)]*\)' $dir/app.log)' after the app's first query (no tracker before it)"
  else
    bad "$passed/$launches;${failures:0:300}"
  fi
}

c_motion-threads() {
  local dir
  run plain mt M5APP_ORDER=motion-threads
  dir=$(run_dir plain mt)
  local first_queries=$(grep -oE 'firstquery ms=\[[^]]*\]' $dir/app.log)
  local updates=$(count_matches 'PASS order.motion-threads.updates' $dir/app.log)
  local first_query=$(count_matches 'PASS order.motion-threads.firstquery' $dir/app.log)
  local tracking=$(count_matches 'tracking started by' $dir/app.log)

  if [[ $updates == 1 && $first_query == 1 && $tracking == 1 ]]; then
    ok "all 3 managers got updates ($(grep -oE 'counts=\[[^]]*\]' $dir/app.log)), one 'tracking started'," \
      "first queries $first_queries"
  else
    bad "$(app_lines 'order.motion-threads' "$dir" 200) tracking=$tracking"
  fi
}

c_none() {
  local dir
  run plain no M5APP_ORDER=none
  dir=$(run_dir plain no)

  # The tracker (a 0.5 s main-queue timer plus device-orientation notifications) logs 'tracking started by' when
  # motion starts it, or 'tracker starts' for any start. The load-time UI orientation snapshot logs nothing in
  # an app that never starts the tracker.
  local armed=$(count_matches 'LAZY armed' $dir/app.log)
  local tracking=$(count_matches 'tracking started|tracker starts' $dir/app.log)
  local done_lines=$(count_matches DONE $dir/app.log)

  if [[ $armed == 0 && $tracking == 0 && $done_lines == 1 ]]; then
    ok "no armed-by, no tracker start (no main-queue timer, no orientation notifications), DONE"
  else
    bad "armed=$armed tracking=$tracking done=$done_lines"
  fi
}

# motion_end_updates <run dir>: the accelerometer update count of plain's "motion.end counts=[<n>]" line
motion_end_updates() { grep -oE 'motion.end counts=\[[0-9]+\]' $1/app.log | grep -oE '[0-9]+'; }

c_motion-gate() {
  clear_sentinel $PLAIN_BUNDLE

  local dir
  run plain mg M5APP_ORDER=motion SIMCAM_PF_BREAK=1
  dir=$(run_dir plain mg)

  # plain's accelerometer stream runs at 100 Hz and its rate check reads it at 1.5 s (150 updates). More than
  # min_end_updates by the end means the stream kept running after the check.
  local min_end_updates=180
  local end_updates=$(motion_end_updates $dir)
  local markers=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N) $STATE_DIR/preflight-fail-$BUILD(N))
  local removed=$(count_matches 'stand-in removed' $dir/dylib.log)
  local rate=$(count_matches 'PASS order.motion.rate' $dir/app.log)
  local disabled=$(count_matches 'DISABLED - preflight failed' $dir/app.log)
  local done_lines=$(count_matches DONE $dir/app.log)

  if [[ $removed == 0 && $rate == 1 && $disabled -ge 1 && $markers == 0 && $done_lines == 1 ]] &&
    (( ${end_updates:-0} > min_end_updates )); then
    ok "camera stock (forced preflight failure), stand-in kept, stream kept running ($end_updates updates by the" \
      "end)," \
      "no 'stand-in removed', no marker"
  else
    bad "removed=$removed rate=$rate end=$end_updates markers=$markers"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_camera-then-motion() {
  clear_sentinel $PLAIN_BUNDLE

  local dir
  run plain ctm M5APP_ORDER=camera-then-motion SIMCAM_PF_BREAK=1
  dir=$(run_dir plain ctm)
  local markers=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N) $STATE_DIR/preflight-fail-$BUILD(N))
  local no_devices=$(count_matches 'camera-then-motion discovery=0' $dir/app.log)
  local answered=$(count_matches 'PASS order.camera-then-motion' $dir/app.log)

  if [[ $no_devices == 1 && $answered == 3 && $markers == 0 ]]; then
    ok "camera stock (0 devices), then the stand-in answers in the same launch (available, rate, timestamps), no marker"
  else
    bad "$(app_lines 'discovery=|order.camera-then-motion' "$dir" 200) markers=$markers"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# ---------------------------------------------------------------- CoreMotion decided at load from markers
# A planted latched-motion marker instead of SIMCAM_OFF_MOTION gives the same landscape angles, with CoreMotion stock
# and its stock line.
c_orient-nomotion-marker() {
  clear_sentinel $ORIENT_BUNDLE

  local now=$(date +%s)
  local angles_plain angles_marker dir1
  run_plain_orient orient landscapeLeft nmk0
  angles_plain=$(orient_angles $LAST_RUN_DIR)
  plant_marker $STATE_DIR/$ORIENT_BUNDLE/latched-motion-$BUILD motion $(( now + 7200 ))
  run_plain_orient orient landscapeLeft nmk1 P12_MOTION_QUERY=1
  angles_marker=$(orient_angles $LAST_RUN_DIR)
  dir1=$LAST_RUN_DIR
  local hooked=$(count_matches 'motion: hooked' $dir1/app.log)
  local traps=$(count_matches 'one-shot forwarding traps' $dir1/app.log)
  local accel_false=$(count_matches 'INFO motion accel=false' $dir1/app.log)
  local stock_lines=$(motion_stock_lines $dir1)
  local marker_named=$(count_matches "latched-motion-$BUILD: motion until" $dir1/dylib.log)
  local done_lines=$(count_matches 'P12\| DONE' $dir1/app.log)

  if [[ -n $angles_marker && $angles_marker == $angles_plain && $angles_marker != *captureAngle=90* ]] &&
    [[ $hooked == 0 && $traps == 1 && $accel_false == 1 && $stock_lines == 1 && $marker_named == 1 ]] &&
    [[ $done_lines == 1 ]]; then
    ok "marker: '$angles_marker', same as without it; CoreMotion stock (accel=false), one stock line:" \
      "'$(grep -m1 -oE "$STOCK_LINE_RX" $dir1/dylib.log)'"
  else
    bad "marker '$angles_marker' vs none '$angles_plain'; hooked=$hooked traps=$traps" \
      "accel=$(grep -oE 'motion accel=[a-z]+' $dir1/app.log) stocklines=$stock_lines"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

# A REAL preflight failure: the stand-in answers in this launch, preflight-fail is written, and the next motion-only
# launch is stock.
c_camera-then-motion-real() {
  clear_sentinel $PLAIN_BUNDLE

  local pf_marker=$STATE_DIR/preflight-fail-$BUILD
  local now=$(date +%s)
  local dir1 dir2 pf_until
  run plain ctr1 M5APP_ORDER=camera-then-motion SIMCAM_PF_BREAK=1 SIMCAM_TEST_PF_FAIL_REAL=1
  dir1=$(run_dir plain ctr1)
  pf_until=$(mtime $pf_marker)
  run plain ctr2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock
  dir2=$(run_dir plain ctr2)
  local no_devices=$(count_matches 'camera-then-motion discovery=0' $dir1/app.log)
  local answered=$(count_matches 'PASS order.camera-then-motion' $dir1/app.log)
  local pf_named=$(count_matches "preflight-fail-$BUILD: preflight failed until" $dir2/dylib.log)

  # the marker's until (its mtime) is 24 h after the failure, give or take 2 minutes
  local until_s=$(( pf_until - now ))
  local slack_s=120

  if [[ $no_devices == 1 && $answered == 3 && $(motion_stock_lines $dir1) == 0 ]] &&
    (( until_s >= MARKER_LIFE_S - slack_s && until_s <= MARKER_LIFE_S + slack_s )) &&
    motion_stock_once $dir2 motion-only && [[ $pf_named == 1 ]]; then
    ok "same launch: camera stock, stand-in answered; preflight-fail written (until +$(( (pf_until - now) / 3600 ))" \
      "h);" \
      "next motion-only: stock (accel=false, 0 updates), one line '$(grep -m1 -oE "$STOCK_LINE_RX" $dir2/dylib.log)'"
  else
    bad "L1: $(app_lines 'discovery=|order.camera-then-motion' "$dir1" 160) pf until +$(( pf_until - now )) s;" \
      "L2: $(app_lines 'order.motion-only' "$dir2" 160) hooked=$(count_matches 'motion: hooked' $dir2/app.log)" \
      "lines=$(motion_stock_lines $dir2)"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# A FORCED preflight failure leaves no marker, so the next motion-only launch gets the stand-in.
c_motion-after-gate() {
  clear_sentinel $PLAIN_BUNDLE

  local dir1 dir2
  run plain mag1 M5APP_ORDER=motion SIMCAM_PF_BREAK=1
  dir1=$(run_dir plain mag1)
  local markers=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N) $STATE_DIR/preflight-fail-$BUILD(N))
  run plain mag2 M5APP_ORDER=motion-only
  dir2=$(run_dir plain mag2)
  local disabled=$(count_matches 'DISABLED - preflight failed' $dir1/app.log)
  local rate=$(count_matches 'PASS order.motion.rate' $dir1/app.log)

  if [[ $disabled -ge 1 && $rate == 1 && $markers == 0 ]] && motion_standin_answered $dir2 motion-only; then
    ok "motion-gate launch: camera stock, stand-in kept, no marker;" \
      "next motion-only: stand-in (available, rate, timestamps), no stock line"
  else
    bad "gate: disabled=$disabled rate=$rate markers=$markers;" \
      "next: $(app_lines 'order.motion-only' "$dir2" 160) lines=$(motion_stock_lines $dir2)"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# A (real) preflight-fail marker and a camera latch marker each make CoreMotion stock from the first query; a clear
# brings the stand-in back.
c_motion-latched() {
  clear_sentinel $PLAIN_BUNDLE

  local events=$(events_log $PLAIN_BUNDLE)
  local now=$(date +%s)
  local dir0 dir1 dir2 dir3 events_seen pf_result marker_result clear_result
  run plain mla0 SIMCAM_TEST_ACTIVATE_FAIL=preflight SIMCAM_TEST_PF_FAIL_REAL=1 M5APP_EXPECT=stock
  dir0=$(run_dir plain mla0)
  run plain mla1 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock
  dir1=$(run_dir plain mla1)

  if motion_stock_once $dir1 motion-only && [[ $(count_matches "preflight-fail-$BUILD" $dir1/dylib.log) == 1 &&
    -f $STATE_DIR/preflight-fail-$BUILD ]]; then
    pf_result=ok
  else
    pf_result="pf: $(app_lines 'order.motion-only' "$dir1" 120) lines=$(motion_stock_lines $dir1)"
  fi

  clear_sentinel $PLAIN_BUNDLE
  plant_marker $STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD activate $(( now + 7200 ))
  run plain mla2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock
  dir2=$(run_dir plain mla2)
  local marker_named=$(count_matches "latched-$BUILD: activate until" $dir2/dylib.log)

  if motion_stock_once $dir2 motion-only && [[ $marker_named == 1 ]]; then
    marker_result=ok
  else
    marker_result="camera marker: $(app_lines 'order.motion-only' "$dir2" 120) lines=$(motion_stock_lines $dir2)"
  fi

  plant_marker $STATE_DIR/preflight-fail-$BUILD "preflight failed" $(( now + 7200 ))
  events_seen=$(line_count $events)
  run plain mla3 M5APP_ORDER=motion-only SIMCAM_SENTINEL=clear
  dir3=$(run_dir plain mla3)
  local left=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N) $STATE_DIR/preflight-fail-$BUILD(N))
  local clear_lines=$(lines_after $events $events_seen | grep -c ' clear - .*SIMCAM_SENTINEL=clear')

  if motion_standin_answered $dir3 motion-only && [[ $left == 0 && $clear_lines == 1 ]]; then
    clear_result=ok
  else
    clear_result="clear: $(app_lines 'order.motion-only' "$dir3" 120) markers left=$left clear lines=$clear_lines"
  fi

  if [[ $pf_result == ok && $marker_result == ok && $clear_result == ok ]]; then
    ok "real preflight failure -> next motion-only stock (accel=false, 0 updates), exactly one stock line;" \
      "planted latched-$BUILD -> the same, line names it;" \
      "SIMCAM_SENTINEL=clear -> stand-in, markers gone, clear line appended"
  else
    bad "$pf_result | $marker_result | $clear_result"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# The preflight-fail marker expires after 24 h, and a passing preflight deletes it.
c_motion-pf-expiry() {
  clear_sentinel $PLAIN_BUNDLE

  local pf_marker=$STATE_DIR/preflight-fail-$BUILD
  local now=$(date +%s)
  local dir1 dir2 dir3 dir4 dir5 expired_result fresh_result pass_result after_result unlinked_result
  plant_marker $pf_marker "preflight failed" $(( now - 60 ))   # written 24 h + 1 min ago
  run plain mpe1 M5APP_ORDER=motion-only
  dir1=$(run_dir plain mpe1)

  if motion_standin_answered $dir1 motion-only; then
    expired_result=ok
  else
    expired_result="expired: $(app_lines 'order.motion-only' "$dir1" 120) lines=$(motion_stock_lines $dir1)"
  fi

  plant_marker $pf_marker "preflight failed" $(( now + 82800 ))   # fresh (written 1 h ago)
  run plain mpe2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock
  dir2=$(run_dir plain mpe2)

  if motion_stock_once $dir2 motion-only; then
    fresh_result=ok
  else
    fresh_result="fresh (control): $(app_lines 'order.motion-only' "$dir2" 120)"
  fi

  run plain mpe3
  dir3=$(run_dir plain mpe3)

  if [[ $(pass_count $dir3) == "$PLAIN_CHECKS" && ! -f $pf_marker ]]; then
    pass_result=ok
  else
    pass_result="passing camera launch: pass=$(pass_count $dir3) pf=$([[ -f $pf_marker ]] && echo kept || echo gone)"
  fi

  run plain mpe4 M5APP_ORDER=motion-only
  dir4=$(run_dir plain mpe4)

  if motion_standin_answered $dir4 motion-only; then
    after_result=ok
  else
    after_result="after: $(app_lines 'order.motion-only' "$dir4" 120) lines=$(motion_stock_lines $dir4)"
  fi

  # A fresh marker that the same launch's passing preflight unlinks before the first motion call: the stock line
  # still names it with a real until (from the load-time stat), never "?".
  plant_marker $pf_marker "preflight failed" $(( now + 82800 ))
  run plain mpe5 M5APP_ORDER=camera-then-motion M5APP_EXPECT_MOTION=stock
  dir5=$(run_dir plain mpe5)
  local stock_line5=$(grep -m1 -oE 'simcam: CoreMotion is stock this launch \([^)]*\)' $dir5/dylib.log)

  if motion_stock_once $dir5 camera-then-motion && [[ ! -f $pf_marker &&
    $stock_line5 == *"preflight-fail-$BUILD: preflight failed until "[0-9][0-9][0-9][0-9]-* &&
    $stock_line5 != *'?'* ]]; then
    unlinked_result=ok
  else
    unlinked_result="unlinked mid-launch: pf=$([[ -f $pf_marker ]] && echo kept || echo gone)"
    unlinked_result+=" line '$stock_line5' lines=$(motion_stock_lines $dir5)"
  fi

  if [[ $expired_result == ok && $fresh_result == ok && $pass_result == ok && $after_result == ok &&
    $unlinked_result == ok ]]; then
    ok "expired (24 h + 1 min): stand-in; fresh: stock (control); a passing camera launch (14/14) unlinked it;" \
      "next motion-only: stand-in; unlinked by the same launch's preflight before the first motion call:" \
      "'${stock_line5#simcam: CoreMotion is stock this launch }'"
  else
    bad "$expired_result | $fresh_result | $pass_result | $after_result | $unlinked_result"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# motion_crash_seq <tag> [K=V ...]: two launches of plain, N and N+1, that crash in a motion tick with the camera
# armed (the K=V go to both). Sets these globals:
#   MOTION_CRASH_DIR1 MOTION_CRASH_DIR2   the run dirs of N and N+1
#   MOTION_CRASH_COUNTS                   their crash lines, counted as crash_counts prints them
#   MOTION_MARKER_COUNT                   1 if this build's latched-motion marker exists afterwards, else 0
#   MOTION_MARKER_UNTIL                   that marker's mtime, which is its until (0 if there is no marker)
#   MOTION_FIRST_STRIKE                   the epoch time of the first `crash motion` line (empty if there is none)
motion_crash_seq() {
  local tag=$1
  shift
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen
  clear_sentinel $PLAIN_BUNDLE
  events_seen=$(line_count $events)
  run plain $tag-1 M5APP_ORDER=camera-then-motion SIMCAM_TEST_MOTION_FAULT=1 "$@"
  MOTION_CRASH_DIR1=$(run_dir plain $tag-1)
  run plain $tag-2 M5APP_ORDER=camera-then-motion SIMCAM_TEST_MOTION_FAULT=1 "$@"
  MOTION_CRASH_DIR2=$(run_dir plain $tag-2)
  MOTION_CRASH_COUNTS=$(crash_counts $events $events_seen)
  MOTION_MARKER_COUNT=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-motion-$BUILD(N))
  MOTION_MARKER_UNTIL=$(mtime $STATE_DIR/$PLAIN_BUNDLE/latched-motion-$BUILD)
  MOTION_FIRST_STRIKE=$(first_event_time $events $events_seen crash motion)
}

# discovery_count <run dir>: how many cameras plain's camera-then-motion discovery found
discovery_count() { grep -oE 'camera-then-motion discovery=[0-9]+' $1/app.log | grep -oE '[0-9]+$'; }

# Launches N and N+1 crash in the stand-in with the camera armed. They count as `crash motion`, not provider, and
# N+1's crash handler writes the latched-motion marker.
c_motion-crash() {
  motion_crash_seq mc
  local dir1=$MOTION_CRASH_DIR1
  local dir2=$MOTION_CRASH_DIR2
  local engine_ok=0
  local dir3 canary_dir
  local pid1=$(fault_pid $dir1/app.log 'SIMCAM_TEST_MOTION_FAULT: fault')
  local pid2=$(fault_pid $dir2/app.log 'SIMCAM_TEST_MOTION_FAULT: fault')
  crash_reports $PLAIN_PROCESS $CRASH_REPORT_WAIT_S $pid1 $pid2

  # the marker's until minus N's strike: 24 h, give or take 5 s
  local until_minus_strike=$(( MOTION_MARKER_UNTIL - ${MOTION_FIRST_STRIKE:-0} ))
  local strike_slack_s=5   # the marker's until is 24 h after the first strike's own time
  run plain mc-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT_MOTION=stock
  dir3=$(run_dir plain mc-3)
  local devices=$(discovery_count $dir3)
  run canary mc-c
  canary_dir=$(run_dir canary mc-c)
  local armed=$(count_matches 'LAZY armed by' $dir3/dylib.log)
  local marker_named=$(count_matches "latched-motion-$BUILD: motion until" $dir3/dylib.log)
  local canary_summary=$(cat $canary_dir/summary.txt 2>/dev/null)   # no summary: the canary did not finish (a FAIL)

  if [[ $MOTION_CRASH_COUNTS == "crash-motionx2 " && $MOTION_MARKER_COUNT == 1 && -n $pid1 && -n $pid2 ]] &&
    in_range $until_minus_strike $(( MARKER_LIFE_S - strike_slack_s )) $(( MARKER_LIFE_S + strike_slack_s )) &&
    (( ${devices:-0} > 0 )) &&
    [[ $armed == 1 ]] && motion_stock_once $dir3 camera-then-motion &&
    [[ $marker_named == 1 && $canary_summary == *core=PASS* ]]; then
    engine_ok=1
  fi

  local ok_text="N, N+1 (pids $pid1 $pid2): $MOTION_CRASH_COUNTS(no provider), $REPORTS_FOUND crash reports;"
  ok_text+=" N+1's handler wrote latched-motion (until = N's strike + 24 h);"
  ok_text+=" N+2: camera armed ($devices devices), CoreMotion stock from the first query, one line;"
  ok_text+=" canary $(cut -c1-40 $canary_dir/summary.txt)"
  local bad_text="events '$MOTION_CRASH_COUNTS' marker=$MOTION_MARKER_COUNT until-strike=${until_minus_strike}s"
  bad_text+=" fault pids '$pid1' '$pid2' reports=$REPORTS_FOUND;"
  bad_text+=" N+2 discovery=${devices:-?} armed=$armed $(app_lines 'order.camera-then-motion' "$dir3" 120)"
  bad_text+=" lines=$(motion_stock_lines $dir3); canary '$(cut -c1-60 $canary_dir/summary.txt 2>/dev/null)'"
  verdict_reports $engine_ok 2 "$ok_text" "$bad_text"
  clear_sentinel $PLAIN_BUNDLE
  clear_sentinel $CANARY_BUNDLE
}

# In a latched-motion launch, 3 threads make their first motion call at once. There must be exactly one stock line.
c_motion-stock-threads() {
  local failures=
  local passed=0
  local now=$(date +%s)
  local i dir

  for i in 1 2 3; do
    clear_sentinel $PLAIN_BUNDLE
    plant_marker $STATE_DIR/$PLAIN_BUNDLE/latched-motion-$BUILD motion $(( now + 7200 ))
    run plain mst$i M5APP_ORDER=motion-threads M5APP_EXPECT_MOTION=stock
    dir=$(run_dir plain mst$i)
    local updates=$(count_matches 'PASS order.motion-threads.stock.updates' $dir/app.log)
    local hooked=$(count_matches 'motion: hooked' $dir/app.log)
    local done_lines=$(count_matches DONE $dir/app.log)
    if [[ $updates == 1 && $hooked == 0 && $(motion_stock_lines $dir) == 1 && $done_lines == 1 ]]; then
      passed=$((passed + 1))
    else
      failures="$failures $i: $(app_lines 'order.motion-threads' "$dir" 120) lines=$(motion_stock_lines $dir)"
    fi
  done

  if (( passed == 3 )); then
    ok "3/3: three threads' first calls into the forwarding traps at once, 0 updates (stock)," \
      "exactly one stock line each"
  else
    bad "$passed/3 ok;$failures"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_MOTION_MARKERS=1 makes motion ignore markers at load. motion-latched must catch it (the stand-in
# answers).
c_mutation-L58() {
  clear_sentinel $PLAIN_BUNDLE

  local now=$(date +%s)
  local dir
  plant_marker $STATE_DIR/$PLAIN_BUNDLE/latched-motion-$BUILD motion $(( now + 7200 ))
  run plain l58 M5APP_ORDER=motion-only SIMCAM_OFF_MOTION_MARKERS=1
  dir=$(run_dir plain l58)

  if motion_standin_answered $dir motion-only; then
    ok "mutant caught: with a valid latched-motion marker the stand-in still answered" \
      "(motion-latched wants stock + one line)"
  else
    bad "mutant not caught: $(app_lines 'order.motion-only' "$dir" 160) lines=$(motion_stock_lines $dir)"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_MOTION_KEY=1 sets no feature key, so motion crashes count as `provider` and close the camera.
# motion-crash must catch it.
c_mutation-L59() {
  motion_crash_seq l59 SIMCAM_OFF_MOTION_KEY=1
  local dir3
  run plain l59-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT=stock
  dir3=$(run_dir plain l59-3)

  if [[ $MOTION_CRASH_COUNTS == "crash-providerx2 " &&
    $(count_matches 'camera-then-motion discovery=0' $dir3/app.log) == 1 ]]; then
    ok "mutant caught: '$MOTION_CRASH_COUNTS', N+2 camera latched (0 devices; motion-crash wants motion x2 and the" \
      "camera armed)"
  else
    bad "mutant not caught: events '$MOTION_CRASH_COUNTS'; N+2 $(grep -oE 'discovery=[0-9]+' $dir3/app.log)"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_HANDLER_MOTION_MARKER=1 writes no marker at the second strike, so N+2 still gets the stand-in
# (the latch comes one launch late).
c_mutation-L60() {
  motion_crash_seq l60 SIMCAM_OFF_HANDLER_MOTION_MARKER=1
  local dir3
  run plain l60-3 M5APP_ORDER=camera-then-motion
  dir3=$(run_dir plain l60-3)

  if [[ $MOTION_CRASH_COUNTS == "crash-motionx2 " && $MOTION_MARKER_COUNT == 0 ]] &&
    motion_standin_answered $dir3 camera-then-motion; then
    ok "mutant caught: '$MOTION_CRASH_COUNTS', no latched-motion after N+1, N+2 got the stand-in (motion-crash wants" \
      "stock at N+2)"
  else
    bad "mutant not caught: events '$MOTION_CRASH_COUNTS' marker=$MOTION_MARKER_COUNT;" \
      "N+2 $(app_lines 'order.camera-then-motion' "$dir3" 120) lines=$(motion_stock_lines $dir3)"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# ---------------------------------------------------------------- the attitude stand-in
# motion_probe <tag> <probe args...>: one launch of fixtures/motion-probe. PROBE_ENV, if set, holds the K=V to inject
# (one per line). Sets PROBE_DIR to the launch's run dir.
motion_probe() {
  local tag=$1
  shift
  # its output stays in its result dir; the verdict is read from there
  XENV="${PROBE_ENV:-}" $ROOT/fixtures/motion-probe/drive.sh $RUNTIME $TAG-$tag boot "$@" >/dev/null 2>&1
  PROBE_DIR=$SIMCAM_RESULTS/motion-$RUNTIME-$TAG-$tag
}

# attitude_summary <run dir>: the probe's "ATT SUMMARY pass=<n> fail=<n>" line
attitude_summary() { grep -m1 -oE 'ATT SUMMARY pass=[0-9]+ fail=[0-9]+' $1/app.log; }

# attitude_failures <run dir>: the probe's "ATT FAIL" checks, each cut to 120 characters and followed by ";"
attitude_failures() { grep 'ATT FAIL' $1/app.log | sed 's/.*ATT FAIL //' | cut -c1-120 | tr '\n' ';'; }

# attitude, quaternion, rotationMatrix and roll/pitch/yaw in portrait, upside-down and both landscapes: 24 checks in
# each orientation
c_motion-attitude() {
  # The orientation comes from the control file's orientation= line, the line `stuntcam orient` writes (the CLI
  # refuses a sim that the scripts armed). The engine reads it when the camera arms, so the probe makes one camera
  # call first (-armcam 1).
  clear_sentinel $MOTION_BUNDLE
  local control=$SIMCAM_RESULTS/activation-$RUNTIME-$TAG-att-control.txt
  local passed=0
  local failures=
  local evidence=
  local orient dir summary
  local attitude_rx='gravity=\([^)]*\) q=\([^)]*\) roll=[-0-9.]+ pitch=[-0-9.]+ yaw=[-0-9.]+'

  for orient in portrait portraitUpsideDown landscapeLeft landscapeRight; do
    print -r -- "orientation=$orient" > $control
    PROBE_ENV="SIMCAM_CONTROL=$control"$'\n' motion_probe att-$orient -attitude 1 -armcam 1 -expect $orient
    dir=$PROBE_DIR
    summary=$(attitude_summary $dir)
    local hooked=$(count_matches 'motion: hooked 16' $dir/app.log)
    if [[ $summary == 'ATT SUMMARY pass=24 fail=0' && $hooked == 1 ]]; then
      passed=$((passed + 1))
      evidence+=" $orient: $(grep -m1 -oE "$attitude_rx" $dir/app.log),"
      evidence+=" $(grep -m1 -oE 'samples=[0-9]+ in 2 s' $dir/app.log);"
    else
      local att_fails=$(attitude_failures $dir)
      failures="$failures [$orient: ${summary:-no summary} hooked=$hooked $att_fails]"
    fi
  done

  rm -f $control

  if (( passed == 4 )); then
    ok "4/4 orientations, 24/24 checks each (frames, pull start, rate, R = matrix(q) = matrix(angles)," \
      "R*down = gravity, stable 2 s, magneticField, Apple's CMAttitude oracle, multiply, copy, coding as Apple's," \
      "M1: decoded attitudes are the stand-in, multiply/isEqual with dm.attitude):$evidence"
  else
    bad "$passed/4;$failures"
  fi

  clear_sentinel $MOTION_BUNDLE
}

# In a latched-motion launch the newer selectors go through the forwarding traps and get stock answers.
c_motion-attitude-stock() {
  clear_sentinel $MOTION_BUNDLE

  local now=$(date +%s)
  local dir0 dir1

  # stock CoreMotion, no traps: the reference answers
  PROBE_ENV="SIMCAM_OFF_MOTION=1"$'\n' motion_probe atts0 -attitude stock
  dir0=$PROBE_DIR
  plant_marker $STATE_DIR/$MOTION_BUNDLE/latched-motion-$BUILD motion $(( now + 7200 ))
  PROBE_ENV= motion_probe atts1 -attitude stock
  dir1=$PROBE_DIR

  # availableAttitudeReferenceFrames, and the reference frame after a start that delivered nothing, as stock
  # CoreMotion answers them and as they come through the traps
  local frames_stock=$(grep -m1 -oE ' frames=[0-9]+' $dir0/app.log)
  local frames_trap=$(grep -m1 -oE ' frames=[0-9]+' $dir1/app.log)
  local frame_stock=$(grep -m1 -oE 'samples=0 frame=[0-9]+' $dir0/app.log)
  local frame_trap=$(grep -m1 -oE 'samples=0 frame=[0-9]+' $dir1/app.log)
  local summary=$(attitude_summary $dir1)
  local hooked=$(count_matches 'motion: hooked' $dir1/app.log)
  local traps=$(count_matches '16 one-shot forwarding traps' $dir1/app.log)
  local stock_lines=$(motion_stock_lines $dir1)

  if [[ $summary == 'ATT SUMMARY pass=3 fail=0' && $hooked == 0 && $traps == 1 && $stock_lines == 1 ]] &&
    [[ -n $frames_stock && $frames_trap == $frames_stock && $frames_trap != ' frames=1' && -n $frame_stock &&
    $frame_trap == $frame_stock ]]; then
    ok "16 traps, one stock line; availableAttitudeReferenceFrames forwarded:$frames_trap (stock:$frames_stock, the" \
      "stand-in answers 1);" \
      "reference-frame starts (queue and pull) delivered nothing, attitudeReferenceFrame $frame_trap as stock"
  else
    bad "$summary hooked=$hooked traps=$traps lines=$stock_lines frames trap:$frames_trap stock:$frames_stock" \
      "'$frame_trap' vs '$frame_stock'"
  fi

  clear_sentinel $MOTION_BUNDLE
}
