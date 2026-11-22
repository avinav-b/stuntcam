#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# memcycle.sh <rt> [cycles] [photo|movie|both] — the plain fixture opens and closes the camera <cycles> times
#   (default 50) in one process (fixtures/plain M5APP_MODE=cycle: a new session with a video data output, photo output
#   and preview layer each time) on an armed sim, and asserts that phys_footprint (the app's own task_info reading
#   after each cycle) stays flat:
#     ok == cycles, least-squares slope over cycles 2..n < $MEMCYCLE_MAX_SLOPE_KB (default 600 KB/cycle) and the median
#     footprint of the last 10 cycles − the median of cycles 6–15 < $MEMCYCLE_MAX_GROWTH_KB (default 12288 KB).
#   Both limits sit ~5x below the session leak described below (~3200 KB/cycle) and well above the fixed engine's
#   noise (26.3: ~4 MB swing between cycles 10 and 50).
#   Variants: plain (open/close only; the torture's), photo (+ one photo per cycle), movie (+ a 1 s movie), both. The
#   photo/movie variants idle 1.5 s after each cycle (encodes finish asynchronously; 26.3 is slow) to
#   keep the samples clean.
#   The leak it guards against: without the fix every session leaks its video data output's conversion pool + VT
#   transfer session, +3.1–3.2 MB per cycle (26 → 189 MB over 50 on 26.3). UDID: $SIMCAM_UDID_<rt>. One line on
#   stdout:
#     memcycle <rt> <variant> PASS|FAIL ok=<ok>/<n> fp1=<KB> fp10=<KB> fpN=<KB> growth=<KB, medians> slope=<KB/cycle>
#   (exit 1 on FAIL). The app's output: results/memcycle-<rt>-<variant>.txt.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=$1
N=${2:-50}
VARIANT=${3:-plain}
need_udid "$0" "$RUNTIME" UDID long

BUNDLE=dev.simcam.spike.m10plain   # fixtures/plain (m10: the milestone that added it; the id is data and stays)
OUT=$SIMCAM_RESULTS/memcycle-$RUNTIME-$VARIANT.txt

(( N >= 25 )) ||
  { echo "$0: need >= 25 cycles (two disjoint 10-cycle windows after cycle 5)" >&2; exit 2; }

typeset -a ENVS
ENVS=(SIMCTL_CHILD_M5APP_MODE=cycle SIMCTL_CHILD_M5APP_CYCLES=$N)
[[ $VARIANT == (photo|both) ]] && ENVS+=(SIMCTL_CHILD_M5APP_CYCLE_PHOTO=1)
[[ $VARIANT == (movie|both) ]] && ENVS+=(SIMCTL_CHILD_M5APP_CYCLE_MOVIE=1)
[[ $VARIANT == plain ]] || ENVS+=(SIMCTL_CHILD_M5APP_CYCLE_IDLE=1.5)

# not running / already granted: nothing to report
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant camera $BUNDLE 2>/dev/null
xcrun simctl privacy $UDID grant microphone $BUNDLE 2>/dev/null

env $ENVS xcrun simctl launch --console-pty $UDID $BUNDLE > $OUT 2>&1 &
LAUNCH_PID=$!
for i in {1..$(( 60 + N * 8 ))}; do   # 1 s polls: 60 s plus 8 s a cycle
  sleep 1
  grep -q "M5APP| DONE" $OUT && break
done

# whatever already ended: nothing to report
xcrun simctl terminate $UDID $BUNDLE 2>/dev/null
sleep 1
kill $LAUNCH_PID 2>/dev/null

# footprint_kb <cycle>: the app's footprint after that cycle, in KB (empty if it didn't log one).
footprint_kb() { grep -oE "MEM cycle=$1 fp_kb=[0-9]+" $OUT | grep -oE '[0-9]+$'; }

CYCLES_OK=$(grep -oE 'INFO cycle ok=[0-9]+' $OUT | grep -oE '[0-9]+$')
SLOPE_KB=$(grep -oE 'slope_kb=-?[0-9.]+' $OUT | cut -d= -f2)
FOOTPRINT1_KB=$(footprint_kb 1)
FOOTPRINT10_KB=$(footprint_kb 10)
FOOTPRINT_LAST_KB=$(footprint_kb $N)

# The growth: the median footprint of the last 10 cycles minus the median of cycles 6-15 (empty unless both windows
# have 10 samples). The fields of "MEM cycle=<c> fp_kb=<kb>" split on "=" and " " are: $3 the cycle, $5 the KB.
GROWTH_KB=$(grep -oE "MEM cycle=[0-9]+ fp_kb=[0-9]+" $OUT | awk -F'[= ]' -v n=$N '
  {
    cycle = $3
    kb = $5
  }
  cycle >= 6 && cycle <= 15 { early[++early_count] = kb }
  cycle > n - 10 { late[++late_count] = kb }
  # median(x, k): the median of x[1..k], which it sorts in place.
  function median(x, k,    swap, p, q) {
    for (p = 1; p <= k; p++)
      for (q = p + 1; q <= k; q++)
        if (x[q] < x[p]) {
          swap = x[p]
          x[p] = x[q]
          x[q] = swap
        }
    return k % 2 ? x[(k + 1) / 2] : (x[k / 2] + x[k / 2 + 1]) / 2
  }
  END {
    if (early_count == 10 && late_count == 10)
      printf "%d", median(late, 10) - median(early, 10)
  }')

RESULT=PASS
[[ ${CYCLES_OK:-0} == $N && -n $SLOPE_KB && -n $GROWTH_KB && -n $FOOTPRINT_LAST_KB ]] || RESULT=FAIL
if [[ $RESULT == PASS ]] &&
    ! (( SLOPE_KB < ${MEMCYCLE_MAX_SLOPE_KB:-600} && GROWTH_KB < ${MEMCYCLE_MAX_GROWTH_KB:-12288} )); then
  RESULT=FAIL
fi

echo "memcycle $RUNTIME $VARIANT $RESULT ok=${CYCLES_OK:-0}/$N fp1=${FOOTPRINT1_KB:--} fp10=${FOOTPRINT10_KB:--}" \
  "fpN=${FOOTPRINT_LAST_KB:--} growth=${GROWTH_KB:--} slope=${SLOPE_KB:--}"
[[ $RESULT == PASS ]]
