#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# memcycle.sh <rt> [cycles] [photo|movie|both] — the plain fixture opens and closes the camera <cycles> times (default 50) in
#   one process (fixtures/plain M5APP_MODE=cycle: a new session with a video data output, photo output and preview layer each
#   time) on an armed sim, and asserts that phys_footprint (the app's own task_info reading after each cycle) stays flat:
#     ok == cycles, least-squares slope over cycles 2..n < $MEMCYCLE_MAX_SLOPE_KB (default 600 KB/cycle) and the median
#     footprint of the last 10 cycles − the median of cycles 6–15 < $MEMCYCLE_MAX_GROWTH_KB (default 12288 KB).
#   Both limits sit ~5x below the L75 leak (~3200 KB/cycle) and well above the fixed engine's noise (26.3: ~4 MB swing
#   between cycles 10 and 50).
#   Variants: plain (open/close only; the torture's), photo (+ one photo per cycle), movie (+ a 1 s movie), both. The
#   photo/movie variants idle 1.5 s after each cycle (encodes finish asynchronously; 26.3 is slow) to keep the samples clean.
#   Ledger L75: without the fix every session leaks its video data output's conversion pool + VT transfer session,
#   +3.1–3.2 MB per cycle (26 → 189 MB over 50 on 26.3). UDID: $SIMCAM_UDID_<rt>. One line on stdout:
#     memcycle <rt> <variant> PASS|FAIL ok=<ok>/<n> fp1=<KB> fp10=<KB> fpN=<KB> growth=<KB, medians> slope=<KB/cycle>
#   (exit 1 on FAIL). The app's output: results/memcycle-<rt>-<variant>.txt.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh; RT=$1; N=${2:-50}; VAR=${3:-plain}
need_udid "$0" "$RT" UDID long
B=dev.simcam.spike.m10plain; OUT=$SIMCAM_RESULTS/memcycle-$RT-$VAR.txt; (( N >= 25 )) ||
  { echo "$0: need >= 25 cycles (two disjoint 10-cycle windows after cycle 5)" >&2; exit 2; }
typeset -a E; E=(SIMCTL_CHILD_M5APP_MODE=cycle SIMCTL_CHILD_M5APP_CYCLES=$N)
[[ $VAR == (photo|both) ]] && E+=(SIMCTL_CHILD_M5APP_CYCLE_PHOTO=1); [[ $VAR == (movie|both) ]] && E+=(SIMCTL_CHILD_M5APP_CYCLE_MOVIE=1)
[[ $VAR == plain ]] || E+=(SIMCTL_CHILD_M5APP_CYCLE_IDLE=1.5)
xcrun simctl terminate $UDID $B 2>/dev/null; xcrun simctl privacy $UDID grant camera $B 2>/dev/null
xcrun simctl privacy $UDID grant microphone $B 2>/dev/null
env $E xcrun simctl launch --console-pty $UDID $B > $OUT 2>&1 &
LP=$!
for i in {1..$(( 60 + N * 8 ))}; do sleep 1; grep -q "M5APP| DONE" $OUT && break; done
xcrun simctl terminate $UDID $B 2>/dev/null; sleep 1; kill $LP 2>/dev/null
fp() { grep -oE "MEM cycle=$1 fp_kb=[0-9]+" $OUT | grep -oE '[0-9]+$'; }
OK=$(grep -oE 'INFO cycle ok=[0-9]+' $OUT | grep -oE '[0-9]+$'); SL=$(grep -oE 'slope_kb=-?[0-9.]+' $OUT | cut -d= -f2)
F1=$(fp 1); F10=$(fp 10); FN=$(fp $N)
G=$(grep -oE "MEM cycle=[0-9]+ fp_kb=[0-9]+" $OUT | awk -F'[= ]' -v n=$N '{c=$3; f=$5} c>=6&&c<=15{a[++i]=f} c>n-10{b[++j]=f}
  function med(x,k,  t,p,q){for(p=1;p<=k;p++)for(q=p+1;q<=k;q++)if(x[q]<x[p]){t=x[p];x[p]=x[q];x[q]=t} return k%2?x[(k+1)/2]:(x[k/2]+x[k/2+1])/2}
  END{if(i==10&&j==10)printf "%d", med(b,10)-med(a,10)}')
R=PASS
[[ ${OK:-0} == $N && -n $SL && -n $G && -n $FN ]] || R=FAIL
[[ $R == PASS ]] && (( SL < ${MEMCYCLE_MAX_SLOPE_KB:-600} && G < ${MEMCYCLE_MAX_GROWTH_KB:-12288} )) || R=FAIL
echo "memcycle $RT $VAR $R ok=${OK:-0}/$N fp1=${F1:--} fp10=${F10:--} fpN=${FN:--} growth=${G:--} slope=${SL:--}"
[[ $R == PASS ]]
