# SPDX-License-Identifier: Apache-2.0
# activation/motion.sh — the CoreMotion stand-in: at load, decided from markers, its crashes, the attitude API.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
c_motion-only() {
  local n=${MOTION_N:-10} i d okn=0 bad1=
  for i in {1..$n}; do
    run plain mo$i M5APP_ORDER=motion-only; d=$(res plain mo$i)
    local order=$(grep -nE 'M5APP\| order=motion-only' $d/app.log | head -1 | cut -d: -f1) tr=$(grep -nE 'tracking started by' $d/app.log | head -1 | cut -d: -f1)
    if [[ $(cnt 'PASS order.motion-only' $d/app.log) == 3 && $(cnt 'LAZY armed' $d/app.log) == 0 && -n $tr &&
      -n $order ]] && (( tr > order )) && [[ $(cnt DONE $d/app.log) == 1 ]]; then okn=$((okn + 1))
    else bad1="$bad1 [$i: pass=$(cnt 'PASS order.motion-only' $d/app.log) armed=$(cnt 'LAZY armed' $d/app.log) tracking@$tr order@$order]"; fi
  done
  (( okn == n )) &&
    ok "$n/$n: stand-in answers (available, rate, timestamps), never armed, '$(grep -m1 -oE 'tracking started by [^)]*\)' $d/app.log)' after the app's first query (no tracker before it)" ||
    bad "$okn/$n;${bad1:0:300}"
}
c_motion-threads() {
  local d; run plain mt M5APP_ORDER=motion-threads; d=$(res plain mt)
  local q=$(grep -oE 'firstquery ms=\[[^]]*\]' $d/app.log)
  if [[ $(cnt 'PASS order.motion-threads.updates' $d/app.log) == 1 &&
    $(cnt 'PASS order.motion-threads.firstquery' $d/app.log) == 1 && $(cnt 'tracking started by' $d/app.log) == 1 ]]
  then ok "all 3 managers got updates ($(grep -oE 'counts=\[[^]]*\]' $d/app.log)), one 'tracking started', first queries $q"
  else bad "$(app_lines 'order.motion-threads' "$d" 200) tracking=$(cnt 'tracking started by' $d/app.log)"; fi
}
c_none() {
  local d; run plain no M5APP_ORDER=none; d=$(res plain no)
  # the tracker (0.5 s main-queue timer + device-orientation notifications) logs 'tracking started by' (motion start) or
  # 'tracker starts' (any start); the load-time UI orientation snapshot (L57) logs nothing in an app that never starts it
  [[ $(cnt 'LAZY armed' $d/app.log) == 0 && $(cnt 'tracking started|tracker starts' $d/app.log) == 0 &&
    $(cnt DONE $d/app.log) == 1 ]] &&
    ok "no armed-by, no tracker start (no main-queue timer, no orientation notifications), DONE" ||
    bad "armed=$(cnt 'LAZY armed' $d/app.log) tracking=$(cnt 'tracking started|tracker starts' $d/app.log) done=$(cnt DONE $d/app.log)"
}
c_motion-gate() {
  clear_sentinel $BP; local d; run plain mg M5APP_ORDER=motion SIMCAM_PF_BREAK=1; d=$(res plain mg)
  # plain's accelerometer stream runs at 100 Hz and its rate check reads it at 1.5 s (150 updates): more than
  # min_end_updates by the end means the stream kept running after the check
  local min_end_updates=180
  local end=$(grep -oE 'motion.end counts=\[[0-9]+\]' $d/app.log | grep -oE '[0-9]+') mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N))
  if [[ $(cnt 'stand-in removed' $d/dylib.log) == 0 && $(cnt 'PASS order.motion.rate' $d/app.log) == 1 &&
    $(cnt 'DISABLED - preflight failed' $d/app.log) -ge 1 && $mk == 0 && $(cnt DONE $d/app.log) == 1 ]] &&
    (( ${end:-0} > min_end_updates ))
  then ok "camera stock (forced preflight failure), stand-in kept, stream kept running ($end updates by the end), no 'stand-in removed', no marker"
  else bad "removed=$(cnt 'stand-in removed' $d/dylib.log) rate=$(cnt 'PASS order.motion.rate' $d/app.log) end=$end markers=$mk"; fi
  clear_sentinel $BP
}
c_camera-then-motion() {
  clear_sentinel $BP; local d; run plain ctm M5APP_ORDER=camera-then-motion SIMCAM_PF_BREAK=1; d=$(res plain ctm)
  local mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N))
  if [[ $(cnt 'camera-then-motion discovery=0' $d/app.log) == 1 && $(cnt 'PASS order.camera-then-motion' $d/app.log) == 3 && $mk == 0 ]]
  then ok "camera stock (0 devices), then the stand-in answers in the same launch (available, rate, timestamps), no marker"
  else bad "$(app_lines 'discovery=|order.camera-then-motion' "$d" 200) markers=$mk"; fi
  clear_sentinel $BP
}
# ---------------------------------------------------------------- CoreMotion decided at load from markers
c_orient-nomotion-marker() {   # a planted latched-motion marker instead of SIMCAM_OFF_MOTION: the same landscape angles, CoreMotion stock with its line
  clear_sentinel $BO; local now=$(date +%s) a0 a1 d1
  p12 orient landscapeLeft nmk0; a0=$(angles $PD)
  plant_marker $STATE/$BO/latched-motion-$BUILD motion $(( now + 7200 ))
  p12 orient landscapeLeft nmk1 P12_MOTION_QUERY=1; a1=$(angles $PD); d1=$PD
  if [[ -n $a1 && $a1 == $a0 && $a1 != *captureAngle=90* && $(cnt 'motion: hooked' $d1/app.log) == 0 && $(cnt 'one-shot forwarding traps' $d1/app.log) == 1 &&
        $(cnt 'INFO motion accel=false' $d1/app.log) == 1 && $(mstock $d1) == 1 && $(cnt "latched-motion-$BUILD: motion until" $d1/dylib.log) == 1 && $(cnt 'P12\| DONE' $d1/app.log) == 1 ]]
  then ok "marker: '$a1', same as without it; CoreMotion stock (accel=false), one stock line: '$(grep -m1 -oE 'CoreMotion is stock this launch \([^)]*\)' $d1/dylib.log)'"
  else bad "marker '$a1' vs none '$a0'; hooked=$(cnt 'motion: hooked' $d1/app.log) traps=$(cnt 'one-shot forwarding traps' $d1/app.log) accel=$(grep -oE 'motion accel=[a-z]+' $d1/app.log) stocklines=$(mstock $d1)"
  fi
  clear_sentinel $BO
}
c_camera-then-motion-real() {   # a REAL preflight failure: stand-in this launch, preflight-fail written, the next motion-only is stock
  clear_sentinel $BP; local d1 d2 pf=$STATE/preflight-fail-$BUILD now=$(date +%s) m
  run plain ctr1 M5APP_ORDER=camera-then-motion SIMCAM_PF_BREAK=1 SIMCAM_TEST_PF_FAIL_REAL=1; d1=$(res plain ctr1); m=$(mtime $pf)
  run plain ctr2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d2=$(res plain ctr2)
  if [[ $(cnt 'camera-then-motion discovery=0' $d1/app.log) == 1 &&
    $(cnt 'PASS order.camera-then-motion' $d1/app.log) == 3 && $(mstock $d1) == 0 ]] &&
     (( m - now >= 86400 - 120 && m - now <= 86400 + 120 )) && mstockok $d2 motion-only &&
       [[ $(cnt "preflight-fail-$BUILD: preflight failed until" $d2/dylib.log) == 1 ]]
  then ok "same launch: camera stock, stand-in answered; preflight-fail written (until +$(( (m - now) / 3600 )) h); next motion-only: stock (accel=false, 0 updates), one line '$(grep -m1 -oE 'CoreMotion is stock this launch \([^)]*\)' $d2/dylib.log)'"
  else bad "L1: $(app_lines 'discovery=|order.camera-then-motion' "$d1" 160) pf until +$(( m - now )) s; L2: $(app_lines 'order.motion-only' "$d2" 160) hooked=$(cnt 'motion: hooked' $d2/app.log) lines=$(mstock $d2)"
  fi
  clear_sentinel $BP
}
c_motion-after-gate() {   # a FORCED preflight failure leaves no marker: the next motion-only gets the stand-in
  clear_sentinel $BP; local d1 d2
  run plain mag1 M5APP_ORDER=motion SIMCAM_PF_BREAK=1; d1=$(res plain mag1)
  local mk=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N))
  run plain mag2 M5APP_ORDER=motion-only; d2=$(res plain mag2)
  if [[ $(cnt 'DISABLED - preflight failed' $d1/app.log) -ge 1 && $(cnt 'PASS order.motion.rate' $d1/app.log) == 1 &&
    $mk == 0 ]] && mstand $d2 motion-only
  then ok "motion-gate launch: camera stock, stand-in kept, no marker; next motion-only: stand-in (available, rate, timestamps), no stock line"
  else bad "gate: disabled=$(cnt 'DISABLED - preflight failed' $d1/app.log) rate=$(cnt 'PASS order.motion.rate' $d1/app.log) markers=$mk; next: $(app_lines 'order.motion-only' "$d2" 160) lines=$(mstock $d2)"
  fi
  clear_sentinel $BP
}
c_motion-latched() {   # preflight-fail (real) and a camera latch marker each stock CoreMotion from the first query; clear brings it back
  clear_sentinel $BP; local e=$(ev $BP) d0 d1 d2 d3 now=$(date +%s) n0 r1 r2 r3
  run plain mla0 SIMCAM_TEST_ACTIVATE_FAIL=preflight SIMCAM_TEST_PF_FAIL_REAL=1 M5APP_EXPECT=stock; d0=$(res plain mla0)
  run plain mla1 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d1=$(res plain mla1)
  mstockok $d1 motion-only && [[ $(cnt "preflight-fail-$BUILD" $d1/dylib.log) == 1 &&
    -f $STATE/preflight-fail-$BUILD ]] && r1=ok ||
    r1="pf: $(app_lines 'order.motion-only' "$d1" 120) lines=$(mstock $d1)"
  clear_sentinel $BP; plant_marker $STATE/$BP/latched-$BUILD activate $(( now + 7200 ))
  run plain mla2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d2=$(res plain mla2)
  mstockok $d2 motion-only && [[ $(cnt "latched-$BUILD: activate until" $d2/dylib.log) == 1 ]] && r2=ok ||
    r2="camera marker: $(app_lines 'order.motion-only' "$d2" 120) lines=$(mstock $d2)"
  plant_marker $STATE/preflight-fail-$BUILD "preflight failed" $(( now + 7200 )); n0=$(nl $e)
  run plain mla3 M5APP_ORDER=motion-only SIMCAM_SENTINEL=clear; d3=$(res plain mla3)
  local left=$(nf $STATE/$BP/latched-*(N) $STATE/preflight-fail-$BUILD(N)) cl=$(since $e $n0 | grep -c ' clear - .*SIMCAM_SENTINEL=clear')
  mstand $d3 motion-only && [[ $left == 0 && $cl == 1 ]] && r3=ok ||
    r3="clear: $(app_lines 'order.motion-only' "$d3" 120) markers left=$left clear lines=$cl"
  if [[ $r1 == ok && $r2 == ok && $r3 == ok ]]
  then ok "real preflight failure -> next motion-only stock (accel=false, 0 updates), exactly one stock line; planted latched-$BUILD -> the same, line names it; SIMCAM_SENTINEL=clear -> stand-in, markers gone, clear line appended"
  else bad "$r1 | $r2 | $r3"; fi
  clear_sentinel $BP
}
c_motion-pf-expiry() {   # preflight-fail expires after 24 h; a passing preflight deletes it
  clear_sentinel $BP; local pf=$STATE/preflight-fail-$BUILD now=$(date +%s) d1 d2 d3 d4 d5 r1 r2 r3 r4 r5
  plant_marker $pf "preflight failed" $(( now - 60 ))   # written 24 h + 1 min ago
  run plain mpe1 M5APP_ORDER=motion-only; d1=$(res plain mpe1); mstand $d1 motion-only && r1=ok ||
    r1="expired: $(app_lines 'order.motion-only' "$d1" 120) lines=$(mstock $d1)"
  plant_marker $pf "preflight failed" $(( now + 82800 ))   # fresh (written 1 h ago)
  run plain mpe2 M5APP_ORDER=motion-only M5APP_EXPECT_MOTION=stock; d2=$(res plain mpe2); mstockok $d2 motion-only &&
    r2=ok || r2="fresh (control): $(app_lines 'order.motion-only' "$d2" 120)"
  run plain mpe3; d3=$(res plain mpe3); [[ $(pass_n $d3) == 14 && ! -f $pf ]] && r3=ok ||
    r3="passing camera launch: pass=$(pass_n $d3) pf=$([[ -f $pf ]] && echo kept || echo gone)"
  run plain mpe4 M5APP_ORDER=motion-only; d4=$(res plain mpe4); mstand $d4 motion-only && r4=ok ||
    r4="after: $(app_lines 'order.motion-only' "$d4" 120) lines=$(mstock $d4)"
  # a fresh marker that the same launch's passing preflight unlinks before the first motion call: the stock
  # line still names it with a real until (from the load-time stat), never "?"
  plant_marker $pf "preflight failed" $(( now + 82800 ))
  run plain mpe5 M5APP_ORDER=camera-then-motion M5APP_EXPECT_MOTION=stock; d5=$(res plain mpe5)
  local l5=$(grep -m1 -oE 'simcam: CoreMotion is stock this launch \([^)]*\)' $d5/dylib.log)
  mstockok $d5 camera-then-motion && [[ ! -f $pf &&
    $l5 == *"preflight-fail-$BUILD: preflight failed until "[0-9][0-9][0-9][0-9]-* && $l5 != *'?'* ]] && r5=ok ||
    r5="unlinked mid-launch: pf=$([[ -f $pf ]] && echo kept || echo gone) line '$l5' lines=$(mstock $d5)"
  if [[ $r1 == ok && $r2 == ok && $r3 == ok && $r4 == ok && $r5 == ok ]]
  then ok "expired (24 h + 1 min): stand-in; fresh: stock (control); a passing camera launch (14/14) unlinked it; next motion-only: stand-in; unlinked by the same launch's preflight before the first motion call: '${l5#simcam: CoreMotion is stock this launch }'"
  else bad "$r1 | $r2 | $r3 | $r4 | $r5"; fi
  clear_sentinel $BP
}
# motion_crash_seq <tag> [K=V ...]: launches N, N+1 crash in a motion tick (camera armed). Sets:
#   MC_D1 MC_D2  their run dirs                MC_EV  their crash lines, "<kind>-<feature>x<count> ..." (sorted)
#   MC_MK        1 if latched-motion exists     MC_MKT its mtime (the marker's until)
#   MC_C1        the time of the first `crash motion` line
motion_crash_seq() {
  local t=$1; shift; local e=$(ev $BP) n0; clear_sentinel $BP; n0=$(nl $e)
  run plain $t-1 M5APP_ORDER=camera-then-motion SIMCAM_TEST_MOTION_FAULT=1 "$@"; MC_D1=$(res plain $t-1)
  run plain $t-2 M5APP_ORDER=camera-then-motion SIMCAM_TEST_MOTION_FAULT=1 "$@"; MC_D2=$(res plain $t-2)
  MC_EV=$(since $e $n0 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' ')
  MC_MK=$(nf $STATE/$BP/latched-motion-$BUILD(N)); MC_MKT=$(mtime $STATE/$BP/latched-motion-$BUILD)
  MC_C1=$(since $e $n0 | awk '$3 == "crash" && $4 == "motion" {print $1; exit}')
}
c_motion-crash() {   # crashes in the stand-in at N, N+1 (camera armed): `crash motion`, not provider; N+1's handler writes latched-motion
  motion_crash_seq mc; local d1=$MC_D1 d2=$MC_D2 d3 dc eng=0
  local p1=$(fault_pid $d1/app.log 'SIMCAM_TEST_MOTION_FAULT: fault') p2=$(fault_pid $d2/app.log 'SIMCAM_TEST_MOTION_FAULT: fault'); crash_reports m5app 30 $p1 $p2
  local ut=$(( MC_MKT - ${MC_C1:-0} ))
  run plain mc-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT_MOTION=stock; d3=$(res plain mc-3)
  local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $d3/app.log | grep -oE '[0-9]+$')
  run canary mc-c; dc=$(res canary mc-c)
  if [[ $MC_EV == "crash-motionx2 " && $MC_MK == 1 && -n $p1 && -n $p2 ]] && (( ut >= 86400 - 5 && ut <= 86400 + 5 )) && (( ${disc:-0} > 0 )) &&
     [[ $(cnt 'LAZY armed by' $d3/dylib.log) == 1 ]] && mstockok $d3 camera-then-motion &&
       [[ $(cnt "latched-motion-$BUILD: motion until" $d3/dylib.log) == 1 &&
       $(cat $dc/summary.txt 2>/dev/null) == *core=PASS* ]]; then eng=1; fi
  verdict_reports $eng 2 "N, N+1 (pids $p1 $p2): $MC_EV(no provider), $CR crash reports; N+1's handler wrote latched-motion (until = N's strike + 24 h); N+2: camera armed ($disc devices), CoreMotion stock from the first query, one line; canary $(cut -c1-40 $dc/summary.txt)" \
    "events '$MC_EV' marker=$MC_MK until-strike=${ut}s fault pids '$p1' '$p2' reports=$CR; N+2 discovery=${disc:-?} armed=$(cnt 'LAZY armed by' $d3/dylib.log) $(app_lines 'order.camera-then-motion' "$d3" 120) lines=$(mstock $d3); canary '$(cut -c1-60 $dc/summary.txt 2>/dev/null)'"
  clear_sentinel $BP; clear_sentinel $BC
}
c_motion-stock-threads() {   # a latched-motion launch whose 3 threads make their first motion call at once: exactly one stock line
  local i d r= okn=0 now=$(date +%s)
  for i in 1 2 3; do
    clear_sentinel $BP; plant_marker $STATE/$BP/latched-motion-$BUILD motion $(( now + 7200 ))
    run plain mst$i M5APP_ORDER=motion-threads M5APP_EXPECT_MOTION=stock; d=$(res plain mst$i)
    if [[ $(cnt 'PASS order.motion-threads.stock.updates' $d/app.log) == 1 && $(cnt 'motion: hooked' $d/app.log) == 0 &&
      $(mstock $d) == 1 && $(cnt DONE $d/app.log) == 1 ]]; then okn=$((okn + 1))
    else r="$r $i: $(app_lines 'order.motion-threads' "$d" 120) lines=$(mstock $d)"; fi
  done
  (( okn == 3 )) &&
    ok "3/3: three threads' first calls into the forwarding traps at once, 0 updates (stock), exactly one stock line each" ||
    bad "$okn/3 ok;$r"
  clear_sentinel $BP
}
c_mutation-L58() {   # SIMCAM_OFF_MOTION_MARKERS=1: motion ignores markers at load: motion-latched must catch it (the stand-in answers)
  clear_sentinel $BP; local d now=$(date +%s); plant_marker $STATE/$BP/latched-motion-$BUILD motion $(( now + 7200 ))
  run plain l58 M5APP_ORDER=motion-only SIMCAM_OFF_MOTION_MARKERS=1; d=$(res plain l58)
  if mstand $d motion-only
  then ok "mutant caught: with a valid latched-motion marker the stand-in still answered (motion-latched wants stock + one line)"
  else bad "mutant not caught: $(app_lines 'order.motion-only' "$d" 160) lines=$(mstock $d)"; fi
  clear_sentinel $BP
}
c_mutation-L59() {   # SIMCAM_OFF_MOTION_KEY=1: no feature key: motion crashes are `provider` and close the camera: motion-crash must catch it
  motion_crash_seq l59 SIMCAM_OFF_MOTION_KEY=1; local d3
  run plain l59-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT=stock; d3=$(res plain l59-3)
  if [[ $MC_EV == "crash-providerx2 " && $(cnt 'camera-then-motion discovery=0' $d3/app.log) == 1 ]]
  then ok "mutant caught: '$MC_EV', N+2 camera latched (0 devices; motion-crash wants motion x2 and the camera armed)"
  else bad "mutant not caught: events '$MC_EV'; N+2 $(grep -oE 'discovery=[0-9]+' $d3/app.log)"; fi
  clear_sentinel $BP
}
c_mutation-L60() {   # SIMCAM_OFF_HANDLER_MOTION_MARKER=1: the second strike writes no marker: N+2 still gets the stand-in (one launch late)
  motion_crash_seq l60 SIMCAM_OFF_HANDLER_MOTION_MARKER=1; local d3
  run plain l60-3 M5APP_ORDER=camera-then-motion; d3=$(res plain l60-3)
  if [[ $MC_EV == "crash-motionx2 " && $MC_MK == 0 ]] && mstand $d3 camera-then-motion
  then ok "mutant caught: '$MC_EV', no latched-motion after N+1, N+2 got the stand-in (motion-crash wants stock at N+2)"
  else bad "mutant not caught: events '$MC_EV' marker=$MC_MK; N+2 $(app_lines 'order.camera-then-motion' "$d3" 120) lines=$(mstock $d3)"; fi
  clear_sentinel $BP
}
# ---------------------------------------------------------------- the attitude stand-in
mprobe() {   # mprobe <tag> <probe args...>: one fixtures/motion-probe launch (XENV from MPX, one K=V per line); sets MD
  local t=$1; shift
  XENV="${MPX:-}" $ROOT/fixtures/motion-probe/drive.sh $RT $TAG-$t boot "$@" >/dev/null 2>&1; MD=$SIMCAM_RESULTS/motion-$RT-$TAG-$t
}
att_sum() { grep -m1 -oE 'ATT SUMMARY pass=[0-9]+ fail=[0-9]+' $1/app.log; }
c_motion-attitude() {   # attitude, quaternion, rotationMatrix, roll/pitch/yaw in portrait, upside-down and both landscapes: 24 checks each
  # The orientation comes from the control file's orientation= line, the line `stuntcam orient` writes (the CLI refuses a sim
  # the scripts armed); the engine reads it when the camera arms, so the probe makes one camera call first (-armcam 1).
  clear_sentinel $BM; local o d c=$SIMCAM_RESULTS/activation-$RT-$TAG-att-control.txt okn=0 r= e= s
  for o in portrait portraitUpsideDown landscapeLeft landscapeRight; do
    print -r -- "orientation=$o" > $c
    MPX="SIMCAM_CONTROL=$c"$'\n' mprobe att-$o -attitude 1 -armcam 1 -expect $o; d=$MD; s=$(att_sum $d)
    if [[ $s == 'ATT SUMMARY pass=24 fail=0' && $(cnt 'motion: hooked 16' $d/app.log) == 1 ]]; then okn=$((okn + 1))
      e="$e $o: $(grep -m1 -oE 'gravity=\([^)]*\) q=\([^)]*\) roll=[-0-9.]+ pitch=[-0-9.]+ yaw=[-0-9.]+' $d/app.log), $(grep -m1 -oE 'samples=[0-9]+ in 2 s' $d/app.log);"
    else r="$r [$o: ${s:-no summary} hooked=$(cnt 'motion: hooked 16' $d/app.log) $(grep 'ATT FAIL' $d/app.log | sed 's/.*ATT FAIL //' | cut -c1-120 | tr '\n' ';')]"
    fi
  done
  rm -f $c
  (( okn == 4 )) &&
    ok "4/4 orientations, 24/24 checks each (frames, pull start, rate, R = matrix(q) = matrix(angles), R*down = gravity, stable 2 s, magneticField, Apple's CMAttitude oracle, multiply, copy, coding as Apple's, M1: decoded attitudes are the stand-in, multiply/isEqual with dm.attitude):$e" ||
    bad "$okn/4;$r"
  clear_sentinel $BM
}
c_motion-attitude-stock() {   # a latched-motion launch: the new selectors go through the forwarding traps and get stock answers
  clear_sentinel $BM; local now=$(date +%s) d0 d1
  MPX="SIMCAM_OFF_MOTION=1"$'\n' mprobe atts0 -attitude stock; d0=$MD   # stock CoreMotion, no traps: the reference answers
  plant_marker $STATE/$BM/latched-motion-$BUILD motion $(( now + 7200 ))
  MPX= mprobe atts1 -attitude stock; d1=$MD
  local f0=$(grep -m1 -oE ' frames=[0-9]+' $d0/app.log) f1=$(grep -m1 -oE ' frames=[0-9]+' $d1/app.log) r0=$(grep -m1 -oE 'samples=0 frame=[0-9]+' $d0/app.log) r1=$(grep -m1 -oE 'samples=0 frame=[0-9]+' $d1/app.log)
  if [[ $(att_sum $d1) == 'ATT SUMMARY pass=3 fail=0' && $(cnt 'motion: hooked' $d1/app.log) == 0 && $(cnt '16 one-shot forwarding traps' $d1/app.log) == 1 && $(mstock $d1) == 1 &&
        -n $f0 && $f1 == $f0 && $f1 != ' frames=1' && -n $r0 && $r1 == $r0 ]]
  then ok "16 traps, one stock line; availableAttitudeReferenceFrames forwarded:$f1 (stock:$f0, the stand-in answers 1); reference-frame starts (queue and pull) delivered nothing, attitudeReferenceFrame $r1 as stock"
  else bad "$(att_sum $d1) hooked=$(cnt 'motion: hooked' $d1/app.log) traps=$(cnt '16 one-shot forwarding traps' $d1/app.log) lines=$(mstock $d1) frames trap:$f1 stock:$f0 '$r1' vs '$r0'"
  fi
  clear_sentinel $BM
}
