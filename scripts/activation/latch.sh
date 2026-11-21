# SPDX-License-Identifier: Apache-2.0
# activation/latch.sh — sentinel latches: hang strikes, latched-* markers, their decay and clearing, the 17.5 container-tmp fallback.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
# latch_seq <prefix> <slow spec>: launches L1, L2 slow; L3, L4 normal; L5 clear. Sets, for the act-latch* cases:
#   L3 L4 L5  their run dirs     H   count of 'hang activate' lines since L1     PH  the distinct phase= values since L1
#   M3 MK3    the mtime and the content of this build's latched marker after L3
latch_seq() {
  local p=$1 s=$2 e=$(ev $BP) n0; clear_sentinel $BP; n0=$(nl $e)
  run plain $p-1 SIMCAM_TEST_SLOW_ACTIVATE_MS=$s M5APP_EXPECT=stock
  run plain $p-2 SIMCAM_TEST_SLOW_ACTIVATE_MS=$s M5APP_EXPECT=stock
  run plain $p-3 ${L3EXTRA:-} M5APP_EXPECT=stock
  M3=$(mtime $STATE/$BP/latched-$BUILD); MK3=$(cat $STATE/$BP/latched-$BUILD 2>/dev/null)
  run plain $p-4 M5APP_EXPECT=stock
  H=$(since $e $n0 | grep -c 'hang activate'); PH=$(since $e $n0 | grep -oE 'phase=[a-z]+' | sort -u | tr '\n' ' ')
  run plain $p-5 SIMCAM_SENTINEL=clear
  L3=$(res plain $p-3); L4=$(res plain $p-4); L5=$(res plain $p-5)
}
c_act-latch() {
  run plain la-0 SIMCAM=0 M5APP_EXPECT=stock; local f0=$(fc "$(res plain la-0)")   # stock AVF's own first-call time (no engine)
  latch_seq la 1500
  local f3=$(fc $L3) f4=$(fc $L4) mk5=$(nf $STATE/$BP/latched-*(N))
  if [[ $H == 2 && $(cnt 'PREFLIGHT' $L3/app.log) == 0 && $(cnt 'latched after' $L3/dylib.log) == 1 && $M3 -gt 0 ]] && (( f3 < 50 )) &&
     [[ $(cnt 'TIMING gate entered' $L4/app.log) == 0 && $(cnt 'latched until' $L4/dylib.log) == 1 ]] &&
       (( f4 < f0 + 5 )) && [[ $(pass_n $L5) == 14 && $mk5 == 0 ]]
  then ok "L1+L2 2 hangs ($PH); L3 latched in PREPARE 1 (no preflight, ${f3} ms, marker '$MK3'); L4 marker at load, no prepare, ${f4} ms (stock AVF alone: ${f0} ms), one 'latched until'; L5 clear 14/14, markers gone"
  else bad "hangs=$H L3: preflight=$(cnt PREFLIGHT $L3/app.log) latched=$(cnt 'latched after' $L3/dylib.log) marker=$M3 fc=$f3; L4: gate=$(cnt 'TIMING gate entered' $L4/app.log) until=$(cnt 'latched until' $L4/dylib.log) fc=$f4 (stock $f0); L5 pass=$(pass_n $L5) markers=$mk5"
  fi
  clear_sentinel $BP
}
c_act-cli-clear() {   # the real CLI's clear unlatches (this build's marker lives only next to the events.log in use)
  local e=$(ev $BP) T=$(tmpdir $BP) cli=$ROOT/cli/bin/stuntcam n0 d3 d4 ms mt ls1 ls2
  clear_sentinel $BP; n0=$(nl $e)
  run plain cc-1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain cc-2 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain cc-3 M5APP_EXPECT=stock; d3=$(res plain cc-3)   # latched in PREPARE 1: writes this build's marker
  ms=$(nf $STATE/$BP/latched-$BUILD(N)); mt=$(nf $T/latched-$BUILD(N))
  ls1=$($cli sentinel ls --udid $U --bundle $BP 2>&1 | grep -iE 'latch' | head -1)
  $cli sentinel clear --udid $U --bundle $BP > $SIMCAM_RESULTS/activation-$RT-$TAG-cliclear.txt 2>&1; local rc=$?
  ls2=$($cli sentinel ls --udid $U --bundle $BP 2>&1 | grep -ciE 'latched')
  run plain cc-4; d4=$(res plain cc-4)
  if [[ $(since $e $n0 | grep -c ' hang activate') == 2 && $(cnt 'latched after' $d3/dylib.log) == 1 && $ms == 1 && $mt == 0 && -n $ls1 && $rc == 0 && $ls2 == 0 &&
        $(cnt 'latched until' $d4/dylib.log) == 0 && $(cnt 'LAZY armed by' $d4/dylib.log) == 1 && $(pass_n $d4) == 14 ]]
  then ok "2 hangs; L3 latched, marker in the state dir only (container tmp: none); ls before: '${ls1:0:90}'; cli/bin/stuntcam sentinel clear (rc 0); ls after: not latched; next launch armed 14/14"
  else bad "hangs=$(since $e $n0 | grep -c ' hang activate') latched=$(cnt 'latched after' $d3/dylib.log) marker state=$ms tmp=$mt ls1='${ls1:0:80}' rc=$rc ls2=$ls2; next: until=$(cnt 'latched until' $d4/dylib.log) armed=$(cnt 'LAZY armed by' $d4/dylib.log) pass=$(pass_n $d4)"
  fi
  clear_sentinel $BP
}
c_act-latch-phase() {
  # @sentinel: L3 also sleeps after the parse, so it times out too, but its latch decision still writes the marker
  L3EXTRA="SIMCAM_TEST_SLOW_ACTIVATE_MS=1500@sentinel" latch_seq lps 1500@sentinel
  local hs=$H phs=$PH m3=$M3 g4=$(cnt 'TIMING gate entered' $L4/app.log) u4=$(cnt 'latched until' $L4/dylib.log)
  latch_seq lpr 1500@resolve
  local hr=$H phr=$PH pf3=$(cnt PREFLIGHT $L3/app.log) l3=$(cnt 'latched after' $L3/dylib.log) u4r=$(cnt 'latched until' $L4/dylib.log)
  if [[ $hs == 3 && $phs == "phase=sentinel " && $m3 -gt 0 && $g4 == 0 && $u4 == 1 && $hr == 2 &&
    $phr == "phase=resolve " && $pf3 == 0 && $l3 == 1 && $u4r == 1 ]]
  then ok "@sentinel: 3 hangs (L3 too) phase=sentinel, L3's marker written, L4 no prepare; @resolve: 2 hangs phase=resolve, L3 latched without preflight, L4 marker-stock"
  else bad "@sentinel hangs=$hs '$phs' marker=$m3 L4gate=$g4 until=$u4; @resolve hangs=$hr '$phr' L3pf=$pf3 latched=$l3 until=$u4r"; fi
  clear_sentinel $BP
}
c_act-decay() {
  local e=$(ev $BP) mx now=$(date +%s) mk=$STATE/$BP/latched-$BUILD d1 d2 m2
  clear_sentinel $BP; mx=$(maxact $e)
  plant $BP "$(( now - 90000 )) $(( mx + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant $BP "$(( now - 90000 )) $(( mx + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  print activate > $mk; setmtime $mk $(( now - 3600 ))
  run plain dc1; d1=$(res plain dc1)
  local aged="pass=$(pass_n $d1) latched=$(cnt 'latched after' $d1/dylib.log) marker=$([[ -f $mk ]] && echo kept || echo deleted)"
  clear_sentinel $BP; now=$(date +%s); mx=$(maxact $e)
  plant $BP "$(( now - 3600 )) $(( mx + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant $BP "$(( now - 3600 )) $(( mx + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  print activate > $mk; setmtime $mk $(( now - 3600 ))
  run plain dc2 M5APP_EXPECT=stock; d2=$(res plain dc2); m2=$(mtime $mk)
  local hrs=$(( (m2 - now) / 360 ))   # tenths of an hour
  if [[ $aged == "pass=14 latched=0 marker=deleted" && $(cnt 'latched after' $d2/dylib.log) == 1 ]] && (( hrs >= 225 && hrs <= 235 ))
  then ok "aged (25 h): $aged, armed; control (1 h): re-latched, marker until +$(( hrs / 10 )).$(( hrs % 10 )) h"
  else bad "aged: $aged; control: latched=$(cnt 'latched after' $d2/dylib.log) marker until +${hrs} tenths h"; fi
  clear_sentinel $BP
}
c_act-marker-multi() {
  local e=$(ev $BP) mx now=$(date +%s) mk=$STATE/$BP/latched-$BUILD d1 d2 m c
  clear_sentinel $BP; mx=$(maxact $e)
  plant $BP "$(( now - 7200 )) $(( mx + 1 )) crash photo $BUILD signal=11"; plant $BP "$(( now - 7200 )) $(( mx + 2 )) crash photo $BUILD signal=11"
  plant $BP "$now $(( mx + 3 )) crash session $BUILD signal=11"; plant $BP "$now $(( mx + 4 )) crash session $BUILD signal=11"
  run plain mm1 M5APP_EXPECT=stock; c=$(cat $mk 2>/dev/null); m=$(mtime $mk)
  run plain mm2 M5APP_EXPECT=stock; d2=$(res plain mm2)
  local line=$(grep -m1 'latched until' $d2/dylib.log | cut -c1-200)
  if [[ $c == *session* && $c == *photo* && $line == *session* && $line == *photo* ]] && (( m - now >= 86400 - 120 && m - now <= 86400 + 120 ))
  then ok "marker '$c', until = session expiry (+$(( (m - now) / 3600 )) h, not photo's +22 h); next launch: '${line#*simcam: }'"
  else bad "marker '$c' until +$(( m - now )) s; line '$line'"; fi
  clear_sentinel $BP
}
c_act-swapback() {
  local e=$(ev $BP) mx now A_LDR=$(cat $ROOT/build/current_loader.txt) B_LDR BB d2 d3 gone
  ( cd $ROOT && EXTRA=-O2 ./build.sh >/dev/null 2>&1 ); B_LDR=$(<$ROOT/build/current_loader.txt); BB=${B_LDR:h:t}
  ( cd $ROOT && ./build.sh >/dev/null 2>&1 )
  [[ "$(<$ROOT/build/current_loader.txt)" == $A_LDR && $BB != $BUILD ]] || { bad "could not make a second build (A=$BUILD B=$BB)"; return; }
  clear_sentinel $BP; now=$(date +%s); mx=$(maxact $e)
  plant $BP "$now $(( mx + 1 )) hang activate $BUILD ms=1000 phase=preflight"
  plant $BP "$now $(( mx + 2 )) hang activate $BUILD ms=1000 phase=preflight"
  run plain sb1 M5APP_EXPECT=stock
  local mA=$([[ -f $STATE/$BP/latched-$BUILD ]] && echo yes || echo no)
  mx=$(maxact $e); for i in {1..20}; do plant $BP "$now $(( mx + i )) activate - $BB traced=0"; done
  $ROOT/scripts/swap.sh $U $B_LDR >/dev/null; run plain sb2; d2=$(res plain sb2)
  gone=$([[ -f $STATE/$BP/latched-$BUILD ]] && echo no || echo yes)
  $ROOT/scripts/swap.sh $U $A_LDR >/dev/null; run plain sb3; d3=$(res plain sb3)
  if [[ $mA == yes && $gone == yes && $(cnt 'removed stale marker' $d2/dylib.log) -ge 1 && $(pass_n $d3) == 14 ]]
  then ok "A ($BUILD) latched with marker; 20 activations of B ($BB); B's prepare removed latched-A; A then armed 14/14"
  else bad "markerA=$mA removedByB=$gone ('$(grep -m1 'stale marker' $d2/dylib.log | cut -c1-80)') A-after pass=$(pass_n $d3)"; fi
  clear_sentinel $BP
}
c_act-sentinel-off() {
  local mk=$STATE/$BP/latched-$BUILD now=$(date +%s) d m0 m1
  clear_sentinel $BP; mkdir -p ${mk:h}; print activate > $mk; setmtime $mk $(( now + 3600 )); m0=$(mtime $mk)
  run plain sof SIMCAM_SENTINEL=0; d=$(res plain sof); m1=$(mtime $mk)
  if [[ $(pass_n $d) == 14 && $(cnt 'LAZY armed by' $d/dylib.log) == 1 && $m0 == $m1 ]]
  then ok "valid marker ignored: armed 14/14; marker untouched (no marker written)"
  else bad "pass=$(pass_n $d) armed=$(cnt 'LAZY armed by' $d/dylib.log) marker mtime $m0 -> $m1"; fi
  rm -f $mk; clear_sentinel $BP
}
c_act-markers-175() {
  local td=$(tmpdir $BP) now=$(date +%s) d1 d2
  block_state; mkdir -p $td; print activate > $td/latched-$BUILD; setmtime $td/latched-$BUILD $(( now + 3600 ))
  run plain mk0 M5APP_EXPECT=stock; local d0=$(res plain mk0)
  run plain mk1 SIMCAM_SENTINEL=clear; d1=$(res plain mk1)
  local gone=$([[ -f $td/latched-$BUILD ]] && echo no || echo yes)
  run plain mk2; d2=$(res plain mk2)
  unblock_state
  if [[ $(cnt 'latched until' $d0/dylib.log) == 1 && $gone == yes && $(pass_n $d2) == 14 && $(cnt 'events=.*/tmp/simcam-state/' $d2/dylib.log) == 1 ]]
  then ok "a container-tmp marker is read at load (stock, 'latched until'); clear removed it; the next launch armed 14/14 with events in the tmp fallback"
  else bad "load stock=$(cnt 'latched until' $d0/dylib.log) marker gone=$gone; normal pass=$(pass_n $d2) tmpEvents=$(cnt 'events=.*/tmp/simcam-state/' $d2/dylib.log)"
  fi
  rm -rf $td   # our own app's container-tmp sentinel state
}
c_act-trim-race() {
  local e=$(ev $BP) mx old=$(( $(date +%s) - 3 * 86400 )) d1 d2 n1 n2 h1 h2
  clear_sentinel $BP; mx=$(maxact $e)
  awk -v s=$(( mx + 1 )) -v t=$old -v b=$BUILD 'BEGIN { for (i = 0; i < 4100; i++) printf "%d %d exit - %s atexit\n", t, s + i, b }' >> $e
  local n0=$(nl $e) hl
  run plain tr1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500@trim M5APP_EXPECT=stock; d1=$(res plain tr1); n1=$(nl $e)
  hl=$(since $e $n0 | grep 'hang activate .*phase=trim'); h1=$(print -r -- "$hl" | grep -c .)
  run plain tr2; d2=$(res plain tr2); n2=$(nl $e); h2=$(grep -cF -- "$hl" $e)
  if [[ $(cnt 'trim skipped' $d1/dylib.log) == 1 && $h1 == 1 &&
    $(cnt 'trimmed .* to the newest 2000' $d2/dylib.log) == 1 && $h2 == 1 ]] && (( n1 > 4000 && n2 < 2200 ))
  then ok "L1: rename skipped (file changed), hang activate phase=trim kept ($n1 lines); L2 trimmed to $n2 lines, the hang line still there"
  else bad "L1 skipped=$(cnt 'trim skipped' $d1/dylib.log) hang=$h1 lines=$n1; L2 trimmed=$(cnt 'trimmed' $d2/dylib.log) lines=$n2 hang=$h2"; fi
  clear_sentinel $BP
}
c_act-175-record() {
  local td=$(tmpdir $BP) d3 d4; rm -rf $td; block_state
  run plain r1 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain r2 SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock
  run plain r3 M5APP_EXPECT=stock; d3=$(res plain r3)
  run plain r4 M5APP_EXPECT=stock; d4=$(res plain r4)
  unblock_state
  local h=$(grep -c 'hang activate' $td/events.log 2>/dev/null)
  if [[ $h == 2 && $(cnt 'latched after' $d3/dylib.log) == 1 && -f $td/latched-$BUILD &&
    $(cnt 'latched until' $d4/dylib.log) == 1 && $(cnt 'TIMING gate entered' $d4/app.log) == 0 ]]
  then ok "both hang lines in the container-tmp events.log; 3rd latched (marker in tmp), 4th STOCK from the marker"
  else bad "tmp hangs=${h:-0} 3rd latched=$(cnt 'latched after' $d3/dylib.log) marker=$([[ -f $td/latched-$BUILD ]] && echo yes || echo no) 4th until=$(cnt 'latched until' $d4/dylib.log)"
  fi
  rm -rf $td
}
