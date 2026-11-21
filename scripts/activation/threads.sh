# SPDX-License-Identifier: Apache-2.0
# activation/threads.sh — first camera calls from several threads at once.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
# tsan_ours <app.log>: "<libsimcam frame> <unrestored stack> <no SUMMARY>" — ThreadSanitizer reports that may be the engine's,
# by the first reason that applies. A report runs from its WARNING line to its SUMMARY line; one that a new WARNING or the end
# of the log interrupts first was cut short (the app was killed mid-report) and counts whatever its frames say.
tsan_ours() {
  awk 'function done() { if (!b) return; if (s) l++; else if (f) u++; else if (!m) c++; b = 0 }
    /WARNING: ThreadSanitizer/ { done(); b = 1; s = 0; f = 0; m = 0; next }
    b && /libsimcam/ { s = 1 }
    b && /\[failed to restore the stack\]/ { f = 1 }
    b && /SUMMARY: ThreadSanitizer/ { m = 1; done() }
    END { done(); print l + 0, u + 0, c + 0 }' $1
}
c_threads() {
  clear_sentinel $BP; local n=${THREADS_N:-100} i d okn=0 dl=0 bad1= ts=0 te=0 t e k tk="0 0 0" kl=0 kf=0 kc=0
  for i in {1..$n}; do
    run plain th$i M5APP_ORDER=threads; d=$(res plain th$i)
    # ThreadSanitizer reports (a TSan app + engine: act-tsan's threads x100 half; none on a normal build). A report counts
    # against the case (as possibly ours) if it has a libsimcam frame, a stack TSan could not restore ("[failed to restore
    # the stack]": the other access may be ours), or no SUMMARY line (cut short: the app was killed mid-report). plain's own
    # races (its M9 frame counter: both stacks restored, no libsimcam frame) are counted apart.
    t=$(cnt 'WARNING: ThreadSanitizer' $d/app.log); ts=$((ts + t))
    e=0; (( t )) && { cp $d/app.log $SIMCAM_RESULTS/activation-$RT-$TAG-th$i-tsan.log
      k=$(tsan_ours $d/app.log); e=$(( ${${=k}[1]} + ${${=k}[2]} + ${${=k}[3]} ))
      kl=$((kl + ${${=k}[1]})); kf=$((kf + ${${=k}[2]})); kc=$((kc + ${${=k}[3]})); }
    te=$((te + e))
    if [[ $(cnt DONE $d/app.log) != 1 ]]; then dl=$((dl + 1)); bad1="$bad1 $i:nodone"
    elif (( e )); then bad1="$bad1 ${i}:tsan-engine=$e"
    elif [[ $(cnt 'PASS order.threads ' $d/app.log) == 1 && $(cnt 'LAZY armed by' $d/dylib.log) == 1 && $(pass_n $d) == 15 ]]; then okn=$((okn + 1))
    else bad1="$bad1 $i:$(grep -oE '(PASS|FAIL) order.threads[^|]*' $d/app.log | cut -c1-60)"; fi
    rm -rf $d   # keep results small; a failing run's verdict is in the line
  done
  (( okn == n )) &&
    ok "$n/$n: no deadlock, one armed-by line, all 3 threads saw cameras, 15/15; ThreadSanitizer: $te possibly ours ($kl libsimcam frame, $kf unrestored stack, $kc cut short), $ts in all" ||
    bad "$okn/$n ok, $dl no DONE, TSan $te possibly ours ($kl libsimcam, $kf unrestored, $kc cut short) / $ts all;${bad1:0:300}"
  clear_sentinel $BP
}
c_threads-slow() {
  clear_sentinel $BP; local d1 d2 m1 m2
  run plain ts1 M5APP_ORDER=threads SIMCAM_TEST_SLOW_ACTIVATE_MS=500; d1=$(res plain ts1)
  m1=$(grep -oE 'firstcall\.ms thread=[0-9] ms=-?[0-9]+' $d1/app.log | grep -oE 'ms=-?[0-9]+$' | cut -d= -f2 | tr '\n' ' ')
  clear_sentinel $BP; run plain ts2 M5APP_ORDER=threads SIMCAM_TEST_SLOW_ACTIVATE_MS=1500 M5APP_EXPECT=stock; d2=$(res plain ts2)
  m2=$(grep -oE 'firstcall\.ms thread=[0-9] ms=-?[0-9]+' $d2/app.log | grep -oE 'ms=-?[0-9]+$' | cut -d= -f2 | tr '\n' ' ')
  local okA=1 okB=1 x
  for x in ${=m1}; do in_range $x 450 550 || okA=0; done; for x in ${=m2}; do in_range $x 900 1100 || okB=0; done
  if [[ $okA == 1 && ${#${=m1}} == 3 && $(cnt 'PASS order.threads ' $d1/app.log) == 1 && $okB == 1 && ${#${=m2}} == 3 &&
    $(cnt 'PASS order.threads.stock' $d2/app.log) == 1 ]]
  then ok "500: all armed, per-thread ms $m1; 1500: each returned by t0+1.1 s ($m2), all 3 saw stock"
  else bad "500: ms '$m1' threads=$(cnt 'PASS order.threads ' $d1/app.log); 1500: ms '$m2' stock=$(cnt 'PASS order.threads.stock' $d2/app.log)"; fi
  clear_sentinel $BP
}
