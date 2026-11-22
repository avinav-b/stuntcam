# SPDX-License-Identifier: Apache-2.0
# activation/latch.sh — sentinel latches: hang strikes, latched-* markers, their decay and clearing,
# the 17.5 container-tmp fallback.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# distinct_phases <events.log> <n>: the distinct phase= values after the first <n> lines, sorted, each followed by
# a space
distinct_phases() { lines_after $1 $2 | grep -oE 'phase=[a-z]+' | sort -u | tr '\n' ' '; }

# latch_seq <prefix> <slow spec>: five launches of plain. L1 and L2 have a slow prepare, L3 and L4 are normal, and L5
# clears the sentinel. LATCH_L3_ENV, if set, adds its K=V to L3. Sets these globals for the act-latch* cases:
#   LATCH_DIR3 LATCH_DIR4 LATCH_DIR5   the run dirs of L3, L4 and L5
#   LATCH_HANGS                        how many 'hang activate' lines were written from L1 on
#   LATCH_PHASES                       the distinct phase= values of those lines, sorted, each followed by a space
#   LATCH_MARKER_MTIME                 the mtime of this build's latched marker after L3 (0 if there is none)
#   LATCH_MARKER_TEXT                  the content of that marker (empty if there is none)
latch_seq() {
  local prefix=$1
  local slow=$2
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen
  clear_sentinel $PLAIN_BUNDLE
  events_seen=$(line_count $events)
  run plain $prefix-1 SIMCAM_TEST_SLOW_ACTIVATE_MS=$slow M5APP_EXPECT=stock
  run plain $prefix-2 SIMCAM_TEST_SLOW_ACTIVATE_MS=$slow M5APP_EXPECT=stock
  run plain $prefix-3 ${LATCH_L3_ENV:-} M5APP_EXPECT=stock
  LATCH_MARKER_MTIME=$(mtime $STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD)
  LATCH_MARKER_TEXT=$(cat $STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD 2>/dev/null)   # no marker: empty
  run plain $prefix-4 M5APP_EXPECT=stock
  LATCH_HANGS=$(lines_after $events $events_seen | grep -c 'hang activate')
  LATCH_PHASES=$(distinct_phases $events $events_seen)
  run plain $prefix-5 SIMCAM_SENTINEL=clear
  LATCH_DIR3=$(run_dir plain $prefix-3)
  LATCH_DIR4=$(run_dir plain $prefix-4)
  LATCH_DIR5=$(run_dir plain $prefix-5)
}

c_act-latch() {
  run plain la-0 SIMCAM=0 M5APP_EXPECT=stock
  local stock_ms=$(first_call_ms "$(run_dir plain la-0)")   # stock AVF's own first-call time (no engine)
  latch_seq la 1500
  local first_ms3=$(first_call_ms $LATCH_DIR3)
  local first_ms4=$(first_call_ms $LATCH_DIR4)
  local markers5=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-*(N))
  local preflight3=$(count_matches 'PREFLIGHT' $LATCH_DIR3/app.log)
  local latched3=$(count_matches 'latched after' $LATCH_DIR3/dylib.log)
  local gate4=$(count_matches 'TIMING gate entered' $LATCH_DIR4/app.log)
  local until4=$(count_matches 'latched until' $LATCH_DIR4/dylib.log)
  local pass5=$(pass_count $LATCH_DIR5)

  # L3 latches before its prepare (under 50 ms); L4 answers within 5 ms of stock AVF's own first call
  local latched_max_ms=50
  local stock_slack_ms=5
  if [[ $LATCH_HANGS == 2 && $preflight3 == 0 && $latched3 == 1 && $LATCH_MARKER_MTIME -gt 0 ]] &&
    (( first_ms3 < latched_max_ms )) &&
    [[ $gate4 == 0 && $until4 == 1 ]] && (( first_ms4 < stock_ms + stock_slack_ms )) &&
    [[ $pass5 == $PLAIN_CHECKS && $markers5 == 0 ]]; then
    ok "L1+L2 2 hangs ($LATCH_PHASES); L3 latched in PREPARE 1 (no preflight, ${first_ms3} ms, marker" \
      "'$LATCH_MARKER_TEXT');" \
      "L4 marker at load, no prepare, ${first_ms4} ms (stock AVF alone: ${stock_ms} ms), one 'latched until';" \
      "L5 clear 14/14, markers gone"
  else
    bad "hangs=$LATCH_HANGS L3: preflight=$(count_matches PREFLIGHT $LATCH_DIR3/app.log) latched=$latched3" \
      "marker=$LATCH_MARKER_MTIME fc=$first_ms3;" \
      "L4: gate=$gate4 until=$until4 fc=$first_ms4 (stock $stock_ms); L5 pass=$pass5 markers=$markers5"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# The real CLI's `sentinel clear` unlatches. This build's marker lives only next to the events.log in use.
c_act-cli-clear() {
  local events=$(events_log $PLAIN_BUNDLE)
  local fallback_dir=$(fallback_state_dir $PLAIN_BUNDLE)
  local cli=$ROOT/cli/bin/stuntcam
  local events_seen dir3 dir4 state_markers tmp_markers ls_before ls_after
  clear_sentinel $PLAIN_BUNDLE
  events_seen=$(line_count $events)
  run plain cc-1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain cc-2 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain cc-3 M5APP_EXPECT=stock   # latched in PREPARE 1: writes this build's marker
  dir3=$(run_dir plain cc-3)
  state_markers=$(count_existing $STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD(N))
  tmp_markers=$(count_existing $fallback_dir/latched-$BUILD(N))
  ls_before=$($cli sentinel ls --udid $UDID --bundle $PLAIN_BUNDLE 2>&1 | grep -iE 'latch' | head -1)
  $cli sentinel clear --udid $UDID --bundle $PLAIN_BUNDLE > $SIMCAM_RESULTS/activation-$RUNTIME-$TAG-cliclear.txt 2>&1
  local rc=$?
  ls_after=$($cli sentinel ls --udid $UDID --bundle $PLAIN_BUNDLE 2>&1 | grep -ciE 'latched')
  run plain cc-4
  dir4=$(run_dir plain cc-4)
  local hangs=$(lines_after $events $events_seen | grep -c ' hang activate')
  local latched3=$(count_matches 'latched after' $dir3/dylib.log)
  local until4=$(count_matches 'latched until' $dir4/dylib.log)
  local armed4=$(count_matches 'LAZY armed by' $dir4/dylib.log)
  local pass4=$(pass_count $dir4)

  if [[ $hangs == 2 && $latched3 == 1 && $state_markers == 1 && $tmp_markers == 0 && -n $ls_before && $rc == 0 &&
    $ls_after == 0 ]] &&
    [[ $until4 == 0 && $armed4 == 1 && $pass4 == $PLAIN_CHECKS ]]; then
    ok "2 hangs; L3 latched, marker in the state dir only (container tmp: none); ls before: '${ls_before:0:90}';" \
      "cli/bin/stuntcam sentinel clear (rc 0); ls after: not latched; next launch armed 14/14"
  else
    bad "hangs=$hangs latched=$latched3 marker state=$state_markers tmp=$tmp_markers ls1='${ls_before:0:80}' rc=$rc" \
      "ls2=$ls_after;" \
      "next: until=$until4 armed=$armed4 pass=$pass4"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-latch-phase() {
  # With @sentinel, L3 also sleeps after the parse, so it times out too; its latch decision still writes the marker.
  LATCH_L3_ENV="SIMCAM_TEST_SLOW_ACTIVATE_MS=1500@sentinel" latch_seq lps 1500@sentinel
  local hangs_sentinel=$LATCH_HANGS
  local phases_sentinel=$LATCH_PHASES
  local marker_mtime=$LATCH_MARKER_MTIME
  local gate4=$(count_matches 'TIMING gate entered' $LATCH_DIR4/app.log)
  local until4=$(count_matches 'latched until' $LATCH_DIR4/dylib.log)
  latch_seq lpr 1500@resolve
  local hangs_resolve=$LATCH_HANGS
  local phases_resolve=$LATCH_PHASES
  local preflight3=$(count_matches PREFLIGHT $LATCH_DIR3/app.log)
  local latched3=$(count_matches 'latched after' $LATCH_DIR3/dylib.log)
  local until4_resolve=$(count_matches 'latched until' $LATCH_DIR4/dylib.log)

  if [[ $hangs_sentinel == 3 && $phases_sentinel == "phase=sentinel " && $marker_mtime -gt 0 && $gate4 == 0 &&
    $until4 == 1 ]] &&
    [[ $hangs_resolve == 2 && $phases_resolve == "phase=resolve " && $preflight3 == 0 && $latched3 == 1 &&
    $until4_resolve == 1 ]]; then
    ok "@sentinel: 3 hangs (L3 too) phase=sentinel, L3's marker written, L4 no prepare;" \
      "@resolve: 2 hangs phase=resolve, L3 latched without preflight, L4 marker-stock"
  else
    bad "@sentinel hangs=$hangs_sentinel '$phases_sentinel' marker=$marker_mtime L4gate=$gate4 until=$until4;" \
      "@resolve hangs=$hangs_resolve '$phases_resolve' L3pf=$preflight3 latched=$latched3 until=$until4_resolve"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-decay() {
  local events=$(events_log $PLAIN_BUNDLE)
  local now=$(date +%s)
  local marker=$STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD
  local max_number dir1 dir2 marker_mtime
  clear_sentinel $PLAIN_BUNDLE
  max_number=$(max_activation_number $events)

  # aged: two strikes 25 h ago (outside the 24 h window), and a marker
  plant_event $PLAIN_BUNDLE "$(( now - 90000 )) $(( max_number + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant_event $PLAIN_BUNDLE "$(( now - 90000 )) $(( max_number + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  print activate > $marker
  setmtime $marker $(( now - 3600 ))
  run plain dc1
  dir1=$(run_dir plain dc1)
  local aged="pass=$(pass_count $dir1) latched=$(count_matches 'latched after' $dir1/dylib.log)"
  aged+=" marker=$([[ -f $marker ]] && echo kept || echo deleted)"

  # control: the same two strikes 1 h ago
  clear_sentinel $PLAIN_BUNDLE
  now=$(date +%s)
  max_number=$(max_activation_number $events)
  plant_event $PLAIN_BUNDLE "$(( now - 3600 )) $(( max_number + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant_event $PLAIN_BUNDLE "$(( now - 3600 )) $(( max_number + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  print activate > $marker
  setmtime $marker $(( now - 3600 ))
  run plain dc2 M5APP_EXPECT=stock
  dir2=$(run_dir plain dc2)
  marker_mtime=$(mtime $marker)
  local until_tenths=$(( (marker_mtime - now) / 360 ))   # tenths of an hour
  local latched2=$(count_matches 'latched after' $dir2/dylib.log)

  # the re-latched marker's until: 23 h after now (24 h after the strikes), give or take half an hour
  local until_min_tenths=225
  local until_max_tenths=235
  if [[ $aged == "pass=14 latched=0 marker=deleted" && $latched2 == 1 ]] &&
    (( until_tenths >= until_min_tenths && until_tenths <= until_max_tenths )); then
    ok "aged (25 h): $aged, armed; control (1 h): re-latched, marker until" \
      "+$(( until_tenths / 10 )).$(( until_tenths % 10 )) h"
  else
    bad "aged: $aged; control: latched=$latched2 marker until +${until_tenths} tenths h"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-marker-multi() {
  local events=$(events_log $PLAIN_BUNDLE)
  local now=$(date +%s)
  local marker=$STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD
  local max_number dir1 dir2 marker_mtime content
  clear_sentinel $PLAIN_BUNDLE
  max_number=$(max_activation_number $events)

  # two photo crashes 2 h ago and two session crashes now: one marker naming both features
  plant_event $PLAIN_BUNDLE "$(( now - 7200 )) $(( max_number + 1 )) crash photo $BUILD signal=11"
  plant_event $PLAIN_BUNDLE "$(( now - 7200 )) $(( max_number + 2 )) crash photo $BUILD signal=11"
  plant_event $PLAIN_BUNDLE "$now $(( max_number + 3 )) crash session $BUILD signal=11"
  plant_event $PLAIN_BUNDLE "$now $(( max_number + 4 )) crash session $BUILD signal=11"
  run plain mm1 M5APP_EXPECT=stock
  content=$(cat $marker 2>/dev/null)   # no marker: empty
  marker_mtime=$(mtime $marker)
  run plain mm2 M5APP_EXPECT=stock
  dir2=$(run_dir plain mm2)
  local line=$(grep -m1 'latched until' $dir2/dylib.log | cut -c1-200)

  # the marker's until is the later (session) expiry: 24 h from now, give or take 2 minutes
  local until_s=$(( marker_mtime - now ))
  local slack_s=120
  if [[ $content == *session* && $content == *photo* && $line == *session* && $line == *photo* ]] &&
    (( until_s >= MARKER_LIFE_S - slack_s && until_s <= MARKER_LIFE_S + slack_s )); then
    ok "marker '$content', until = session expiry (+$(( (marker_mtime - now) / 3600 )) h, not photo's +22 h);" \
      "next launch: '${line#*simcam: }'"
  else
    bad "marker '$content' until +$(( marker_mtime - now )) s; line '$line'"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-swapback() {
  local events=$(events_log $PLAIN_BUNDLE)
  local loader_a=$(cat $ROOT/build/current_loader.txt)
  local max_number now loader_b build_b dir2 dir3 gone i

  # Make a second build B (another -O level gives another hash), then build the original A again.
  ( cd $ROOT && EXTRA=-O2 ./build.sh >/dev/null 2>&1 )
  loader_b=$(<$ROOT/build/current_loader.txt)
  build_b=${loader_b:h:t}
  ( cd $ROOT && ./build.sh >/dev/null 2>&1 )

  if ! [[ "$(<$ROOT/build/current_loader.txt)" == $loader_a && $build_b != $BUILD ]]; then
    bad "could not make a second build (A=$BUILD B=$build_b)"
    return
  fi

  clear_sentinel $PLAIN_BUNDLE
  now=$(date +%s)
  max_number=$(max_activation_number $events)
  plant_event $PLAIN_BUNDLE "$now $(( max_number + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant_event $PLAIN_BUNDLE "$now $(( max_number + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  run plain sb1 M5APP_EXPECT=stock
  local marker_a=$([[ -f $STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD ]] && echo yes || echo no)
  max_number=$(max_activation_number $events)

  # A whole window of B's activations: A's two hangs are now outside it.
  for i in {1..$LATCH_WINDOW_ACTIVATIONS}; do
    plant_event $PLAIN_BUNDLE "$now $(( max_number + i )) activate - $build_b traced=0"
  done
  $ROOT/scripts/swap.sh $UDID $loader_b >/dev/null
  run plain sb2
  dir2=$(run_dir plain sb2)
  gone=$([[ -f $STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD ]] && echo no || echo yes)
  $ROOT/scripts/swap.sh $UDID $loader_a >/dev/null
  run plain sb3
  dir3=$(run_dir plain sb3)
  local removed=$(count_matches 'removed stale marker' $dir2/dylib.log)
  local pass3=$(pass_count $dir3)

  if [[ $marker_a == yes && $gone == yes && $removed -ge 1 && $pass3 == $PLAIN_CHECKS ]]; then
    ok "A ($BUILD) latched with marker; $LATCH_WINDOW_ACTIVATIONS activations of B ($build_b);" \
      "B's prepare removed latched-A; A then armed 14/14"
  else
    bad "markerA=$marker_a removedByB=$gone ('$(grep -m1 'stale marker' $dir2/dylib.log | cut -c1-80)') A-after" \
      "pass=$pass3"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-sentinel-off() {
  local marker=$STATE_DIR/$PLAIN_BUNDLE/latched-$BUILD
  local now=$(date +%s)
  local dir mtime_before mtime_after
  clear_sentinel $PLAIN_BUNDLE
  mkdir -p ${marker:h}
  print activate > $marker
  setmtime $marker $(( now + 3600 ))
  mtime_before=$(mtime $marker)
  run plain sof SIMCAM_SENTINEL=0
  dir=$(run_dir plain sof)
  mtime_after=$(mtime $marker)
  local pass=$(pass_count $dir)
  local armed=$(count_matches 'LAZY armed by' $dir/dylib.log)

  if [[ $pass == $PLAIN_CHECKS && $armed == 1 && $mtime_before == $mtime_after ]]; then
    ok "valid marker ignored: armed 14/14; marker untouched (no marker written)"
  else
    bad "pass=$pass armed=$armed marker mtime $mtime_before -> $mtime_after"
  fi

  rm -f $marker
  clear_sentinel $PLAIN_BUNDLE
}

c_act-markers-175() {
  local tmp_state=$(fallback_state_dir $PLAIN_BUNDLE)
  local now=$(date +%s)
  local dir1 dir2
  block_state
  mkdir -p $tmp_state
  print activate > $tmp_state/latched-$BUILD
  setmtime $tmp_state/latched-$BUILD $(( now + 3600 ))
  run plain mk0 M5APP_EXPECT=stock
  local dir0=$(run_dir plain mk0)
  run plain mk1 SIMCAM_SENTINEL=clear
  dir1=$(run_dir plain mk1)
  local gone=$([[ -f $tmp_state/latched-$BUILD ]] && echo no || echo yes)
  run plain mk2
  dir2=$(run_dir plain mk2)
  unblock_state
  local stock0=$(count_matches 'latched until' $dir0/dylib.log)
  local pass2=$(pass_count $dir2)
  local tmp_events=$(count_matches 'events=.*/tmp/simcam-state/' $dir2/dylib.log)

  if [[ $stock0 == 1 && $gone == yes && $pass2 == $PLAIN_CHECKS && $tmp_events == 1 ]]; then
    ok "a container-tmp marker is read at load (stock, 'latched until'); clear removed it;" \
      "the next launch armed 14/14 with events in the tmp fallback"
  else
    bad "load stock=$stock0 marker gone=$gone; normal pass=$pass2 tmpEvents=$tmp_events"
  fi
  rm -rf $tmp_state   # our own app's container-tmp sentinel state
}

c_act-trim-race() {
  local events=$(events_log $PLAIN_BUNDLE)
  local old_epoch=$(( $(date +%s) - 3 * 86400 ))   # three days ago
  local max_number dir1 dir2 lines1 lines2 hangs1 hangs2
  clear_sentinel $PLAIN_BUNDLE
  max_number=$(max_activation_number $events)

  # 4100 old exit lines, over the engine's trim threshold of 4000 lines
  awk -v s=$(( max_number + 1 )) -v t=$old_epoch -v b=$BUILD \
    'BEGIN { for (i = 0; i < 4100; i++) printf "%d %d exit - %s atexit\n", t, s + i, b }' >> $events
  local events_seen=$(line_count $events)
  local hang_line
  run plain tr1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500@trim M5APP_EXPECT=stock
  dir1=$(run_dir plain tr1)
  lines1=$(line_count $events)
  hang_line=$(lines_after $events $events_seen | grep 'hang activate .*phase=trim')
  hangs1=$(print -r -- "$hang_line" | grep -c .)
  run plain tr2
  dir2=$(run_dir plain tr2)
  lines2=$(line_count $events)
  hangs2=$(grep -cF -- "$hang_line" $events)
  local skipped=$(count_matches 'trim skipped' $dir1/dylib.log)
  local trimmed=$(count_matches 'trimmed .* to the newest 2000' $dir2/dylib.log)

  # the engine trims a file over 4000 lines to its newest 2000 (a few more may be written after the trim)
  local trim_threshold_lines=4000
  local trimmed_max_lines=2200
  if [[ $skipped == 1 && $hangs1 == 1 && $trimmed == 1 && $hangs2 == 1 ]] &&
    (( lines1 > trim_threshold_lines && lines2 < trimmed_max_lines )); then
    ok "L1: rename skipped (file changed), hang activate phase=trim kept ($lines1 lines);" \
      "L2 trimmed to $lines2 lines, the hang line still there"
  else
    bad "L1 skipped=$skipped hang=$hangs1 lines=$lines1; L2 trimmed=$(count_matches 'trimmed' $dir2/dylib.log)" \
      "lines=$lines2 hang=$hangs2"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

c_act-175-record() {
  local tmp_state=$(fallback_state_dir $PLAIN_BUNDLE)
  local dir3 dir4
  rm -rf $tmp_state
  block_state
  run plain r1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain r2 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain r3 M5APP_EXPECT=stock
  dir3=$(run_dir plain r3)
  run plain r4 M5APP_EXPECT=stock
  dir4=$(run_dir plain r4)
  unblock_state
  local hangs=$(grep -c 'hang activate' $tmp_state/events.log 2>/dev/null)   # no events.log: empty (reported as 0)
  local latched3=$(count_matches 'latched after' $dir3/dylib.log)
  local until4=$(count_matches 'latched until' $dir4/dylib.log)
  local gate4=$(count_matches 'TIMING gate entered' $dir4/app.log)

  if [[ $hangs == 2 && $latched3 == 1 && -f $tmp_state/latched-$BUILD && $until4 == 1 && $gate4 == 0 ]]; then
    ok "both hang lines in the container-tmp events.log; 3rd latched (marker in tmp), 4th STOCK from the marker"
  else
    bad "tmp hangs=${hangs:-0} 3rd latched=$latched3" \
      "marker=$([[ -f $tmp_state/latched-$BUILD ]] && echo yes || echo no)" \
      "4th until=$until4"
  fi
  rm -rf $tmp_state
}
