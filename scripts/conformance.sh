#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# conformance.sh <rt> [case ...] — targeted checks of the safety and arming paths, on a sim already armed by
#   scripts/boot.sh <rt> stub. Each case runs one or a few launches through scripts/run.sh and asserts on the app, dylib
#   and stub logs; prints "PASS <case> …" / "FAIL <case> …" and exits 1 if any case failed.
#   cases (default: all):
#     kill-switch    SIMCAM=0 per launch: stub logs it, no camera, no crash
#     motion-gate    preflight broken with CoreMotion queried first: the camera is stock, the CoreMotion stand-in
#                    stays for the launch and its stream keeps running, no marker
#     current-bad    `current` = garbage / proto 0 / relative path / missing: one refusal line each, no camera
#     proto-skew     `current` with a newer proto: "needs a newer stub", no camera
#     latch-photo    two hung photo ops latch photo in-process; the next real photo error-completes
#                    (-11800/-16800)
#     crash-latch    two attributed crashes latch the next activation (no camera); SIMCAM_SENTINEL=clear restores it
#   The sentinel is cleared after the cases that latch, and `current` is restored (written back as it was) after the
#   ones that edit it; an exit trap restores it too, so Ctrl-C mid-case doesn't leave the sim with a broken `current`.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=${1:?usage: $0 <rt> [case ...]}
shift
need_udid "$0" "$RUNTIME" UDID short

typeset -a CASES
CASES=("$@")
(( ${#CASES} )) ||
  CASES=(kill-switch motion-gate current-bad proto-skew latch-photo crash-latch)   # ${@:-a b} is one word in zsh

CURRENT_FILE=$SIMCAM_HOME/sims/$UDID/current
if [[ ! -f $CURRENT_FILE ]]; then
  echo "$0: $CURRENT_FILE missing: boot with scripts/boot.sh $RUNTIME stub first" >&2
  exit 2
fi
GOOD_CURRENT=$(<$CURRENT_FILE)

fails=0
TAG=conf-$(date +%H%M%S)

# restore_current: writes `current` back (atomically) if a case left it changed.
restore_current() {
  # no current: not the good one, so it is written below
  [[ $(cat $CURRENT_FILE 2>/dev/null) == $GOOD_CURRENT ]] && return
  print -r -- $GOOD_CURRENT > $CURRENT_FILE.tmp && mv $CURRENT_FILE.tmp $CURRENT_FILE
}
trap restore_current EXIT

# on_interrupt: when interrupted mid-case, also drops the latching cases' sentinel state (a clear launch is too slow
# for a trap), then exits 130.
on_interrupt() {
  local state_dir=$SIMCAM_HOME/sims/$UDID/state
  # the canary's and plain's bundle ids (m10: the milestone that added them; ids are data and stay)
  rm -rf $state_dir/dev.simcam.spike.m10canary $state_dir/dev.simcam.spike.m10plain
  exit 130
}
trap on_interrupt INT TERM

# stublog: the boot's stub log.
stublog() { print -r -- "$(cat $ROOT/build/bootlogdir-$RUNTIME.txt)/stub.log"; }

# run_dir <app> <n>: the result dir of run.sh <app> … <tag>-<n>.
run_dir() { print -r -- "$SIMCAM_RESULTS/$1-$RUNTIME-boot-$TAG-$2"; }

ok() { echo "PASS $1 — $2"; }
bad() {
  echo "FAIL $1 — $2"
  fails=$((fails + 1))
}

run() {   # run <app> <n> [K=V ...]: one run.sh launch in boot mode; no retry, output dropped
  local app=$1 n=$2
  shift 2
  # its output stays in its result dir; the verdict is read from there
  XENV="$*" SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh $app $RUNTIME boot $TAG-$n >/dev/null 2>&1
}

# clear_sentinel <app>: clears that app's sentinel state (it is kept per bundle id).
clear_sentinel() { run $1 clear-$1 SIMCAM_SENTINEL=clear; }

# lines_since <file> <byte offset>: prints the file from that offset on (nothing if it is missing).
lines_since() {
  local file=$1 from=$2
  [[ -f $file ]] && tail -c +$((from + 1)) $file || true
}

# size <file>: its size in bytes (0 if it is missing).
size() { [[ -f $1 ]] && stat -f %z $1 || echo 0; }

for case_name in $CASES; do
  case $case_name in
    kill-switch)
      log_offset=$(size "$(stublog)")
      run canary ks SIMCAM=0
      # a run that wrote no summary gives an empty one, which the case reports
      summary=$(cat "$(run_dir canary ks)"/summary.txt 2>/dev/null)

      if lines_since "$(stublog)" $log_offset | grep -q 'SIMCAM=0, nothing loaded' && [[ $summary == *RESULT* &&
        $summary != *core=PASS* ]]; then
        ok $case_name "stub logged SIMCAM=0, canary ran without a camera"
      else
        bad $case_name "stub/log mismatch: ${summary:0:160}"
      fi
      ;;

    motion-gate)
      run plain mg M5APP_ORDER=motion SIMCAM_PF_BREAK=1
      dir=$(run_dir plain mg)
      state_dir=$SIMCAM_HOME/sims/$UDID/state
      engine_hash=$(<$ROOT/build/current_dylib.txt)
      engine_hash=${engine_hash:h:t}

      # plain's accelerometer stream runs at 100 Hz and its rate check reads it at 1.5 s (150 updates): more than
      # min_end_updates by the end means the stream kept running after the check
      min_end_updates=180
      # a log that is missing counts as no lines in these checks
      end_updates=$(grep -oE 'motion.end counts=\[[0-9]+\]' $dir/app.log 2>/dev/null | grep -oE '[0-9]+')
      markers=$(print -l $state_dir/dev.simcam.spike.m10plain/latched-*(N) $state_dir/preflight-fail-$engine_hash(N) |
        grep -c .)

      if ! grep -q 'stand-in removed' $dir/dylib.log 2>/dev/null && grep -q 'PASS order.motion.rate' $dir/app.log &&
        grep -q 'DISABLED - preflight failed' $dir/app.log &&
        (( ${end_updates:-0} > min_end_updates )) && (( markers == 0 )) && grep -q 'DONE' $dir/app.log; then
        ok $case_name "camera stock, stand-in kept: $end_updates updates by the end, no 'stand-in removed', no marker"
      else
        removed=$(grep -c 'stand-in removed' $dir/dylib.log 2>/dev/null)
        bad $case_name "removed=$removed end=${end_updates:-?} markers=$markers (see $dir)"
      fi
      ;;

    current-bad)
      i=0
      for line in 'abc def' '0 /nonexistent/loader.dylib' '1 relative/loader.dylib' ''; do
        i=$((i + 1))
        log_offset=$(size "$(stublog)")
        if [[ -n $line ]]; then
          print -r -- $line > $CURRENT_FILE
        else
          rm -f $CURRENT_FILE
        fi
        run canary cb$i
        refusals=$(lines_since "$(stublog)" $log_offset |
          grep -cE 'nothing loaded|needs|no current file|empty current|loader failed')
        # a run that wrote no summary gives an empty one, which the case reports
        summary=$(cat "$(run_dir canary cb$i)"/summary.txt 2>/dev/null)
        if (( refusals >= 1 )) && [[ $summary != *core=PASS* ]]; then
          ok "${case_name}[$i]" "'${line:-<missing>}' refused ($refusals line)"
        else
          bad "${case_name}[$i]" "'${line:-<missing>}': $refusals refusal lines, ${summary:0:100}"
        fi
      done
      print -r -- $GOOD_CURRENT > $CURRENT_FILE
      ;;

    proto-skew)
      log_offset=$(size "$(stublog)")
      print -r -- "2 ${GOOD_CURRENT#* }" > $CURRENT_FILE
      run canary ps
      if lines_since "$(stublog)" $log_offset | grep -q 'needs a newer stub'; then
        ok $case_name "newer proto refused"
      else
        bad $case_name "no 'needs a newer stub' line"
      fi
      print -r -- $GOOD_CURRENT > $CURRENT_FILE
      ;;

    latch-photo)
      run plain lp SIMCAM_WD_TEST=hang
      dir=$(run_dir plain lp)
      # a log that is missing counts as no lines in these checks
      if grep -q 'photo latched: this capture error-completes' $dir/dylib.log 2>/dev/null &&
        grep -qE '\-11800' $dir/app.log && grep -q 'DONE' $dir/app.log
      then
        ok $case_name "photo error-completed: $(grep -oE 'code=?-11800[^ ]*' $dir/app.log | head -1)"
      elif ! grep -q 'latched after 2 hangs' $dir/dylib.log 2>/dev/null; then
        bad $case_name "photo never latched before the photo (timing: see $dir/dylib.log)"
      else
        bad $case_name "latched, but the photo did not error-complete (see $dir/app.log)"
      fi
      clear_sentinel plain
      ;;

    crash-latch)
      run canary cl1 SIMCAM_WD_TEST=crash
      run canary cl2 SIMCAM_WD_TEST=crash
      run canary cl3
      # a run that wrote no summary or log gives empty values, which the case reports
      summary3=$(cat "$(run_dir canary cl3)"/summary.txt 2>/dev/null)
      latch_lines3=$(grep -c 'latched after' "$(run_dir canary cl3)"/app.log 2>/dev/null)

      clear_sentinel canary
      run canary cl4
      # a run that wrote no summary gives an empty one, which the case reports
      summary4=$(cat "$(run_dir canary cl4)"/summary.txt 2>/dev/null)

      if (( latch_lines3 >= 1 )) && [[ $summary3 != *core=PASS* && $summary4 == *core=PASS* ]]; then
        ok $case_name "latched after 2 crashes, cleared → PASS"
      else
        bad $case_name "latched-run: ${summary3:0:80} (latch lines $latch_lines3); after clear: ${summary4:0:80}"
      fi
      ;;

    *)
      bad $case_name "unknown case"
      ;;
  esac
done

if (( fails )); then
  echo "$fails case(s) failed"
  exit 1
fi
echo "all ${#CASES} cases passed"
