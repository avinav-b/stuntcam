#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# conformance.sh <rt> [case ...] — targeted checks of the safety and arming paths, on a sim already armed by
#   scripts/boot.sh <rt> stub. Each case runs one or a few launches through scripts/run.sh and asserts on the app, dylib and
#   stub logs; prints "PASS <case> …" / "FAIL <case> …" and exits 1 if any case failed. Ledger entries in brackets.
#   cases (default: all):
#     kill-switch    SIMCAM=0 per launch: stub logs it, no camera, no crash
#     motion-gate    preflight broken with CoreMotion queried first: the camera is stock, the CoreMotion stand-in stays for the
#                    launch and its stream keeps running, no marker [L40; W4 changed it: a failed activation no longer removes
#                    the stand-in mid-run (D2)]
#     current-bad    `current` = garbage / proto 0 / relative path / missing: one refusal line each, no camera [L50]
#     proto-skew     `current` with a newer proto: "needs a newer stub", no camera
#     latch-photo    two hung photo ops latch photo in-process; the next real photo error-completes (-11800/-16800) [L15]
#     crash-latch    two attributed crashes latch the next activation (no camera); SIMCAM_SENTINEL=clear restores it
#   The sentinel is cleared after the cases that latch, and `current` is restored (written back as it was) after the ones that
#   edit it; an exit trap restores it too, so Ctrl-C mid-case doesn't leave the sim with a broken `current`.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh
RT=${1:?usage: $0 <rt> [case ...]}; shift
V=SIMCAM_UDID_$RT; U=${(P)V:-}; [[ -n $U ]] || { echo "$0: set $V" >&2; exit 2; }
typeset -a CASES; CASES=("$@"); (( ${#CASES} )) || CASES=(kill-switch motion-gate current-bad proto-skew latch-photo crash-latch)   # ${@:-a b} is one word in zsh
CUR=$SIMCAM_HOME/sims/$U/current; [[ -f $CUR ]] || { echo "$0: $CUR missing: boot with scripts/boot.sh $RT stub first" >&2; exit 2; }
GOOD=$(<$CUR); fails=0; TAG=conf-$(date +%H%M%S)
restore_current() { [[ $(cat $CUR 2>/dev/null) == $GOOD ]] || { print -r -- $GOOD > $CUR.tmp && mv $CUR.tmp $CUR; }; }
trap restore_current EXIT
# interrupted mid-case: also drop the latching cases' sentinel state (a clear launch is too slow for a trap)
trap 'rm -rf $SIMCAM_HOME/sims/$U/state/dev.simcam.spike.m10canary $SIMCAM_HOME/sims/$U/state/dev.simcam.spike.m10plain; exit 130' INT TERM
stublog() { print -r -- "$(cat $ROOT/build/bootlogdir-$RT.txt)/stub.log"; }
res() { print -r -- "$SIMCAM_RESULTS/$1-$RT-boot-$TAG-$2"; }   # result dir of run.sh <app> … <tag>-<n>
ok() { echo "PASS $1 — $2"; }
bad() { echo "FAIL $1 — $2"; fails=$((fails + 1)); }
run() { local app=$1 n=$2; shift 2; XENV="$*" SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh $app $RT boot $TAG-$n >/dev/null 2>&1; }
clear_sentinel() { run $1 clear-$1 SIMCAM_SENTINEL=clear; }   # sentinel state is per bundle id
lines_since() { local f=$1 from=$2; [[ -f $f ]] && tail -c +$((from + 1)) $f || true; }
size() { [[ -f $1 ]] && stat -f %z $1 || echo 0; }

for c in $CASES; do case $c in
  kill-switch)
    s0=$(size "$(stublog)"); run canary ks SIMCAM=0
    sum=$(cat "$(res canary ks)"/summary.txt 2>/dev/null)
    if lines_since "$(stublog)" $s0 | grep -q 'SIMCAM=0, nothing loaded' && [[ $sum == *RESULT* && $sum != *core=PASS* ]]; then ok $c "stub logged SIMCAM=0, canary ran without a camera"
    else bad $c "stub/log mismatch: ${sum:0:160}"; fi;;
  motion-gate)
    run plain mg M5APP_ORDER=motion SIMCAM_PF_BREAK=1
    d=$(res plain mg)
    st=$SIMCAM_HOME/sims/$U/state; b=$(<$ROOT/build/current_dylib.txt); b=${b:h:t}
    end=$(grep -oE 'motion.end counts=\[[0-9]+\]' $d/app.log 2>/dev/null | grep -oE '[0-9]+')
    mk=$(print -l $st/dev.simcam.spike.m10plain/latched-*(N) $st/preflight-fail-$b(N) | grep -c .)
    if ! grep -q 'stand-in removed' $d/dylib.log 2>/dev/null && grep -q 'PASS order.motion.rate' $d/app.log && grep -q 'DISABLED - preflight failed' $d/app.log &&
       (( ${end:-0} > 180 )) && (( mk == 0 )) && grep -q 'DONE' $d/app.log; then
      ok $c "camera stock, stand-in kept: $end updates by the end, no 'stand-in removed', no marker"
    else bad $c "removed=$(grep -c 'stand-in removed' $d/dylib.log 2>/dev/null) end=${end:-?} markers=$mk (see $d)"; fi;;
  current-bad)
    i=0
    for line in 'abc def' '0 /nonexistent/loader.dylib' '1 relative/loader.dylib' ''; do
      i=$((i + 1)); s0=$(size "$(stublog)")
      if [[ -n $line ]]; then print -r -- $line > $CUR; else rm -f $CUR; fi
      run canary cb$i
      got=$(lines_since "$(stublog)" $s0 | grep -cE 'nothing loaded|needs|no current file|empty current|loader failed')
      sum=$(cat "$(res canary cb$i)"/summary.txt 2>/dev/null)
      if (( got >= 1 )) && [[ $sum != *core=PASS* ]]; then ok "${c}[$i]" "'${line:-<missing>}' refused ($got line)"; else bad "${c}[$i]" "'${line:-<missing>}': $got refusal lines, ${sum:0:100}"; fi
    done
    print -r -- $GOOD > $CUR;;
  proto-skew)
    s0=$(size "$(stublog)"); print -r -- "2 ${GOOD#* }" > $CUR; run canary ps
    if lines_since "$(stublog)" $s0 | grep -q 'needs a newer stub'; then ok $c "newer proto refused"; else bad $c "no 'needs a newer stub' line"; fi
    print -r -- $GOOD > $CUR;;
  latch-photo)
    run plain lp SIMCAM_WD_TEST=hang
    d=$(res plain lp)
    if grep -q 'photo latched: this capture error-completes' $d/dylib.log 2>/dev/null && grep -qE '\-11800' $d/app.log && grep -q 'DONE' $d/app.log; then ok $c "photo error-completed: $(grep -oE 'code=?-11800[^ ]*' $d/app.log | head -1)"
    elif ! grep -q 'latched after 2 hangs' $d/dylib.log 2>/dev/null; then bad $c "photo never latched before the photo (timing: see $d/dylib.log)"
    else bad $c "latched, but the photo did not error-complete (see $d/app.log)"; fi
    clear_sentinel plain;;
  crash-latch)
    run canary cl1 SIMCAM_WD_TEST=crash; run canary cl2 SIMCAM_WD_TEST=crash; run canary cl3
    s3=$(cat "$(res canary cl3)"/summary.txt 2>/dev/null)
    l3=$(grep -c 'latched after' "$(res canary cl3)"/app.log 2>/dev/null)
    clear_sentinel canary; run canary cl4; s4=$(cat "$(res canary cl4)"/summary.txt 2>/dev/null)
    if (( l3 >= 1 )) && [[ $s3 != *core=PASS* && $s4 == *core=PASS* ]]; then ok $c "latched after 2 crashes, cleared → PASS"
    else bad $c "latched-run: ${s3:0:80} (latch lines $l3); after clear: ${s4:0:80}"; fi;;
  *) bad $c "unknown case";;
esac; done
(( fails )) && { echo "$fails case(s) failed"; exit 1; } || echo "all ${#CASES} cases passed"
