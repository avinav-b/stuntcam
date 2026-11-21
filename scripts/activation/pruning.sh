# SPDX-License-Identifier: Apache-2.0
# activation/pruning.sh — pruning of expired preflight-fail markers, and markers being written while that runs.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
c_pf-prune() {   # PREPARE 1 prunes preflight-fail-* past its until (this build and others); another build's fresh one stays
  clear_sentinel $BP
  local now=$(date +%s) d mine=$STATE/preflight-fail-$BUILD old=$STATE/preflight-fail-0000deadbeef fresh=$STATE/preflight-fail-0000cafef00d
  plant_marker $mine "preflight failed" $(( now - 60 )); plant_marker $old "preflight failed" $(( now - 3600 ))
  plant_marker $fresh "preflight failed" $(( now + 3600 ))
  run plain pfp; d=$(res plain pfp)
  local k="mine=$([[ -f $mine ]] && echo kept || echo gone) other-expired=$([[ -f $old ]] && echo kept || echo gone) other-fresh=$([[ -f $fresh ]] && echo kept || echo gone)"
  if [[ $(pass_n $d) == 14 && $k == "mine=gone other-expired=gone other-fresh=kept" &&
    $(cnt 'removed expired marker .*preflight-fail-' $d/dylib.log) == 2 ]]
  then ok "one camera launch (14/14): $k, 2 'removed expired marker' lines"
  else bad "pass=$(pass_n $d) $k removed-lines=$(cnt 'removed expired marker .*preflight-fail-' $d/dylib.log)"; fi
  rm -f $old $fresh; clear_sentinel $BP
}
# pfrace_seq <tag> [K=V ...]: plain writes preflight-fail (a real failure) paused mid-write for 15 s (K=V go to it);
# meanwhile the canary (forced preflight failure: no marker of its own) runs PREPARE 1's prune. Sets:
#   PR_W   paused (plain logged its paused write) or never     PR_RM  how many 'removed expired marker' lines the canary logged for it
#   PR_MK  1 if the marker exists at the end                    PR_UT  its until minus now, in s
#   PR_P1  "gate=<n> before=<f> after=<f>": the canary got past PREPARE 1 (its 'TIMING gate entered' line) while the
#          write's file was there before and after the canary's run: "tmp" (the temp file), "inplace" (L64: the
#          marker itself), or what was missing
pfrace_seq() {
  local t=$1; shift; local mk=$STATE/preflight-fail-$BUILD dc tb ta
  clear_sentinel $BP; clear_sentinel $BC
  bg_run plain $t-a SIMCAM_TEST_ACTIVATE_FAIL=preflight SIMCAM_TEST_PF_FAIL_REAL=1 SIMCAM_TEST_SLOW_MARKER_MS=15000 SIMCAM_ACTIVATE_TIMEOUT_MS=40000 M5APP_EXPECT=stock "$@"
  if wait_for $BGDIR/app.log 'SIMCAM_TEST_SLOW_MARKER_MS: marker write paused' 30; then PR_W=paused
    tb=$(pf_writing $mk)
    run canary $t-b SIMCAM_PF_BREAK=1; dc=$(res canary $t-b)
    ta=$(pf_writing $mk)
    PR_RM=$(cnt "removed expired marker .*preflight-fail-$BUILD" $dc/dylib.log)
    PR_P1="gate=$(cnt 'TIMING gate entered' $dc/dylib.log) before=$tb after=$ta"
  else PR_W=never; PR_RM=?; PR_P1=never; fi
  wait $BGPID
  PR_MK=$(nf $mk(N)); PR_UT=$(( $(mtime $mk) - $(date +%s) ))
}
pf_writing() {   # pf_writing <marker>: "tmp" if a temp file of it exists, "inplace" if only the marker, else "none"
  local -a tm; tm=(${1:h}/.${1:t}.tmp-<->(N))
  (( ${#tm} )) && print tmp || { [[ -f $1 ]] && print inplace || print none; }
}
c_pf-prune-race() {   # a preflight-fail marker another process is writing is never pruned as expired (temp file + rename)
  pfrace_seq prr
  if [[ $PR_W == paused && $PR_RM == 0 && $PR_MK == 1 && $PR_P1 == 'gate=1 before=tmp after=tmp' ]] &&
    (( PR_UT > 86000 ))
  then ok "the canary got past PREPARE 1 (its gate line) while plain's temp file was there, before and after its run (15 s paused write): nothing pruned, the marker landed with until +$(( PR_UT / 3600 )) h"
  else bad "write $PR_W, canary $PR_P1, prune lines=$PR_RM, marker=$PR_MK until-now=${PR_UT}s"; fi
  clear_sentinel $BP; clear_sentinel $BC
}
c_pf-prune-kept() {   # the prune's "kept" branch: a fresh marker renamed over the expired one after PREPARE 1's stat, before
  # its rename aside, is put back, not unlinked. Plain prunes another build's expired preflight-fail, paused 4 s before the
  # rename (SIMCAM_TEST_PRUNE_PAUSE_MS); in the pause the host renames a fresh one (until +1 h) over it, as SCWriteMarker does.
  clear_sentinel $BP; local now=$(date +%s) d w=never m=$STATE/preflight-fail-0000cafef00d tmp=$STATE/.preflight-fail-0000cafef00d.tmp-host
  plant_marker $m "preflight failed" $(( now - 3600 ))
  bg_run plain pfk SIMCAM_TEST_PRUNE_PAUSE_MS=4000 SIMCAM_ACTIVATE_TIMEOUT_MS=40000; d=$BGDIR
  if wait_for $d/app.log 'SIMCAM_TEST_PRUNE_PAUSE_MS: prune of .*preflight-fail-0000cafef00d paused' 30; then
    plant_marker $tmp "preflight failed (fresh)" $(( now + 3600 )); mv -f $tmp $m; w=replaced; fi
  wait $BGPID
  local ut=$(( $(mtime $m) - now )) kept=$(cnt 're-written while being pruned: kept' $d/dylib.log) rm=$(cnt 'removed expired marker .*preflight-fail-0000cafef00d' $d/dylib.log)
  local c=$(head -1 $m 2>/dev/null) left=$(nf $STATE/.preflight-fail-0000cafef00d.tmp-prune-*(N))
  if [[ $w == replaced && $kept == 1 && $rm == 0 && $c == "preflight failed (fresh)" && $ut == 3600 && $left == 0 ]]
  then ok "the fresh marker renamed in during the paused prune was put back: 1 'kept' line, none removed, content '$c', until +$ut s, no temp left"
  else bad "host write $w; kept lines=$kept removed=$rm; marker '$c' until-now=${ut}s; aside files left=$left"; fi
  rm -f $m $tmp; clear_sentinel $BP
}
c_mutation-L64() {   # SIMCAM_OFF_MARKER_RENAME=1: markers written in place again (open O_TRUNC, then futimens): pf-prune-race must catch it
  pfrace_seq l64 SIMCAM_OFF_MARKER_RENAME=1
  if [[ $PR_W == paused && $PR_RM == 1 && $PR_MK == 0 && $PR_P1 == 'gate=1 before=inplace after=none' ]]
  then ok "mutant caught: the canary pruned the marker plain was writing in place (1 'removed expired marker' line, no marker after; $PR_P1)"
  else bad "mutant not caught: write $PR_W, canary $PR_P1, prune lines=$PR_RM, marker=$PR_MK until-now=${PR_UT}s"; fi
  clear_sentinel $BP; clear_sentinel $BC
}
