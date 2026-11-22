# SPDX-License-Identifier: Apache-2.0
# activation/pruning.sh — pruning of expired preflight-fail markers, and markers being written while that runs.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# kept_or_gone <file>: "kept" if it exists, else "gone"
kept_or_gone() { [[ -f $1 ]] && echo kept || echo gone; }

# PREPARE 1 prunes every preflight-fail-* marker past its until, this build's and other builds'. Another build's
# fresh marker stays.
c_pf-prune() {
  clear_sentinel $PLAIN_BUNDLE

  local now=$(date +%s)
  local mine=$STATE_DIR/preflight-fail-$BUILD
  local dir
  local old=$STATE_DIR/preflight-fail-0000deadbeef
  local fresh=$STATE_DIR/preflight-fail-0000cafef00d
  plant_marker $mine "preflight failed" $(( now - 60 ))
  plant_marker $old "preflight failed" $(( now - 3600 ))
  plant_marker $fresh "preflight failed" $(( now + 3600 ))
  run plain pfp
  dir=$(run_dir plain pfp)
  local kept="mine=$(kept_or_gone $mine) other-expired=$(kept_or_gone $old) other-fresh=$(kept_or_gone $fresh)"
  local pass=$(pass_count $dir)
  local removed=$(count_matches 'removed expired marker .*preflight-fail-' $dir/dylib.log)

  if [[ $pass == $PLAIN_CHECKS && $kept == "mine=gone other-expired=gone other-fresh=kept" && $removed == 2 ]]; then
    ok "one camera launch (14/14): $kept, 2 'removed expired marker' lines"
  else
    bad "pass=$pass $kept removed-lines=$removed"
  fi

  rm -f $old $fresh
  clear_sentinel $PLAIN_BUNDLE
}

# pfrace_seq <tag> [K=V ...]: plain hits a real preflight failure and writes the preflight-fail marker, paused for
# 15 s in the middle of the write (the K=V go to plain). Meanwhile the canary runs PREPARE 1's prune; its preflight
# failure is forced, so it writes no marker of its own. Sets these globals:
#   PF_RACE_WRITE          "paused" if plain logged its paused write, else "never"
#   PF_RACE_PRUNED         how many 'removed expired marker' lines the canary logged for this build's marker
#   PF_RACE_MARKER_COUNT   1 if the marker exists at the end, else 0
#   PF_RACE_UNTIL_S        the marker's until minus now, in seconds
#   PF_RACE_CANARY         "gate=<n> before=<file> after=<file>": <n> counts the canary's 'TIMING gate entered'
#                          lines (1 when it got past PREPARE 1). <file> is what pf_writing saw just before and just
#                          after the canary's run: "tmp" (the temp file), "inplace" (the marker itself: the bug of
#                          writing it in place) or "none".
#   When plain never logs its paused write, PF_RACE_WRITE and PF_RACE_CANARY are "never" and PF_RACE_PRUNED is "?".
pfrace_seq() {
  local tag=$1
  shift
  local marker=$STATE_DIR/preflight-fail-$BUILD
  local canary_dir before after
  clear_sentinel $PLAIN_BUNDLE
  clear_sentinel $CANARY_BUNDLE
  bg_run plain $tag-a SIMCAM_TEST_ACTIVATE_FAIL=preflight SIMCAM_TEST_PF_FAIL_REAL=1 SIMCAM_TEST_SLOW_MARKER_MS=15000 \
    SIMCAM_ACTIVATE_TIMEOUT_MS=40000 M5APP_EXPECT=stock "$@"

  if wait_for $BG_DIR/app.log 'SIMCAM_TEST_SLOW_MARKER_MS: marker write paused' 30; then
    PF_RACE_WRITE=paused
    before=$(pf_writing $marker)
    run canary $tag-b SIMCAM_PF_BREAK=1
    canary_dir=$(run_dir canary $tag-b)
    after=$(pf_writing $marker)
    PF_RACE_PRUNED=$(count_matches "removed expired marker .*preflight-fail-$BUILD" $canary_dir/dylib.log)
    PF_RACE_CANARY="gate=$(count_matches 'TIMING gate entered' $canary_dir/dylib.log) before=$before after=$after"
  else
    PF_RACE_WRITE=never
    PF_RACE_PRUNED=?
    PF_RACE_CANARY=never
  fi

  wait $BG_PID
  PF_RACE_MARKER_COUNT=$(count_existing $marker(N))
  PF_RACE_UNTIL_S=$(( $(mtime $marker) - $(date +%s) ))
}

# pf_writing <marker>: "tmp" if a temp file of it exists, "inplace" if only the marker, else "none"
pf_writing() {
  local -a temps
  temps=(${1:h}/.${1:t}.tmp-<->(N))

  if (( ${#temps} )); then
    print tmp
  elif [[ -f $1 ]]; then
    print inplace
  else
    print none
  fi
}

# A preflight-fail marker that another process is writing is never pruned as expired: it is written to a temp file
# and renamed into place.
c_pf-prune-race() {
  pfrace_seq prr

  # the marker that landed is good for 24 h; a few minutes less leaves slack for the run itself
  local slack_s=400
  if [[ $PF_RACE_WRITE == paused && $PF_RACE_PRUNED == 0 && $PF_RACE_MARKER_COUNT == 1 &&
    $PF_RACE_CANARY == 'gate=1 before=tmp after=tmp' ]] &&
    (( PF_RACE_UNTIL_S > MARKER_LIFE_S - slack_s )); then
    ok "the canary got past PREPARE 1 (its gate line) while plain's temp file was there, before and after its run" \
      "(15 s paused write): nothing pruned, the marker landed with until +$(( PF_RACE_UNTIL_S / 3600 )) h"
  else
    bad "write $PF_RACE_WRITE, canary $PF_RACE_CANARY, prune lines=$PF_RACE_PRUNED, marker=$PF_RACE_MARKER_COUNT" \
      "until-now=${PF_RACE_UNTIL_S}s"
  fi

  clear_sentinel $PLAIN_BUNDLE
  clear_sentinel $CANARY_BUNDLE
}

# The prune's "kept" branch: a fresh marker renamed over the expired one after PREPARE 1's stat, but before the prune
# renames it aside, is put back, not unlinked. Plain prunes another build's expired preflight-fail marker and pauses
# 4 s before the rename (SIMCAM_TEST_PRUNE_PAUSE_MS). In the pause the host renames a fresh marker (until +1 h) over
# it, as SCWriteMarker does.
c_pf-prune-kept() {
  clear_sentinel $PLAIN_BUNDLE

  local now=$(date +%s)
  local write=never
  local marker=$STATE_DIR/preflight-fail-0000cafef00d
  local dir
  local tmp=$STATE_DIR/.preflight-fail-0000cafef00d.tmp-host
  plant_marker $marker "preflight failed" $(( now - 3600 ))
  bg_run plain pfk SIMCAM_TEST_PRUNE_PAUSE_MS=4000 SIMCAM_ACTIVATE_TIMEOUT_MS=40000
  dir=$BG_DIR

  if wait_for $dir/app.log 'SIMCAM_TEST_PRUNE_PAUSE_MS: prune of .*preflight-fail-0000cafef00d paused' 30; then
    plant_marker $tmp "preflight failed (fresh)" $(( now + 3600 ))
    mv -f $tmp $marker
    write=replaced
  fi

  wait $BG_PID
  local until_s=$(( $(mtime $marker) - now ))
  local kept=$(count_matches 're-written while being pruned: kept' $dir/dylib.log)
  local removed=$(count_matches 'removed expired marker .*preflight-fail-0000cafef00d' $dir/dylib.log)
  local content=$(head -1 $marker 2>/dev/null)   # no marker: empty
  local left=$(count_existing $STATE_DIR/.preflight-fail-0000cafef00d.tmp-prune-*(N))

  if [[ $write == replaced && $kept == 1 && $removed == 0 ]] &&
    [[ $content == "preflight failed (fresh)" && $until_s == 3600 && $left == 0 ]]; then
    ok "the fresh marker renamed in during the paused prune was put back: 1 'kept' line, none removed," \
      "content '$content', until +$until_s s, no temp left"
  else
    bad "host write $write; kept lines=$kept removed=$removed; marker '$content' until-now=${until_s}s; aside files" \
      "left=$left"
  fi

  rm -f $marker $tmp
  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_MARKER_RENAME=1 writes markers in place again (open with O_TRUNC, then futimens). pf-prune-race
# must catch it.
c_mutation-L64() {
  pfrace_seq l64 SIMCAM_OFF_MARKER_RENAME=1

  if [[ $PF_RACE_WRITE == paused && $PF_RACE_PRUNED == 1 && $PF_RACE_MARKER_COUNT == 0 &&
    $PF_RACE_CANARY == 'gate=1 before=inplace after=none' ]]; then
    ok "mutant caught: the canary pruned the marker plain was writing in place" \
      "(1 'removed expired marker' line, no marker after; $PF_RACE_CANARY)"
  else
    bad "mutant not caught: write $PF_RACE_WRITE, canary $PF_RACE_CANARY, prune lines=$PF_RACE_PRUNED," \
      "marker=$PF_RACE_MARKER_COUNT until-now=${PF_RACE_UNTIL_S}s"
  fi

  clear_sentinel $PLAIN_BUNDLE
  clear_sentinel $CANARY_BUNDLE
}
