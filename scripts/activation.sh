#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# activation.sh <rt> [case ...] — tests bounded lazy activation and CoreMotion's load-time decision on a simulator
# armed by `scripts/boot.sh <rt> stub`.
#
# Output: one line per case, "PASS <case> — <evidence>", "FAIL <case> — <evidence>" or "UNSURE <case> — <evidence>",
# also appended to results/activation-<rt>-<tag>.txt. The last line counts the verdicts. The script exits 1 if any
# case failed; an UNSURE case does not count as a failure.
#
# Cases: with no case named, it runs every case that applies to <rt>.
#   - On 17.5 that is act-slow-ok, act-slow-timeout, act-fail and the -175 cases.
#   - On other versions it is the long list below, plus MOTION_CASES (CoreMotion at load, tracker faults, marker
#     pruning, the attitude stand-in, app-answerable sends) and TRACE_STOP_CASES (the op trace file, the deferred-stop
#     flush, the crash-handler race, a late stop waiter). The trace cases read traces with cli/bin/stuntcam, so build
#     it first.
#   - MOTION_SUITE=1 runs only the motion cases and MOTION_CASES.
#   THREADS_N (default 100) is how many launches the `threads` case makes; MOTION_N (default 10) is the same for
#   `motion-only`.
#
# A timing case that fails is run once more before it counts, because another agent's load can starve the machine.
# Both verdicts and the load average are printed.
#
# Every counted case starts and ends with a cleared sentinel. The script clears it on the host: it appends a clear line
# to the app's events.log and removes its latched-* markers, in the state dir and in the container-tmp fallback. That
# is the state a SIMCAM_SENTINEL=clear launch leaves, without the 3-15 s that launch takes.
#
# Crash reports: a case that wants the host's .ips report for a crash it caused matches reports by pid, never by
# count or mtime.
#   - The pids come from the launch's own log: its fault line "[simcam <pid>] SIMCAM_TEST_*: fault ..." if the case
#     has one, else its first "[simcam <pid>]" line.
#   - A report counts if its process name is the app's, its "pid" is one of those, and it was captured at or after the
#     case started (CASE_START, so a reused pid never matches an older report). tools/ipspid.py looks in
#     DiagnosticReports and in Retired/.
#   - A missing report is waited for up to 30 s; reports have been seen 12 s late.
#   - If every engine-side check passed and a pid still has no report, the case is UNSURE ("no crash report written
#     for pid ..."). macOS stops writing reports for a process after many crashes, so a missing report is host state,
#     not an engine verdict. (Whenever a crash of m5app, the plain app's process, got no report, DiagnosticReports held
#     exactly 25 m5app reports and the rest were in Retired/. Once orient-crash-ensure's second report was missing with
#     only 5 there; the cause is unknown.)
#   - A case that wants NO report (act-slow-timeout) counts only its own launch's pid.
#
# act-traced attaches lldb from the command line (the SB API through scripts/lldb-attach.py, no GUI). act-tsan is not
# here: it needs a ThreadSanitizer build of the app and a boot with a ThreadSanitizer engine.
#
# Layout: this file holds the case lists and the driver. Each case <c> is the function c_<c> in scripts/activation/,
# one file per concern; helpers.sh holds what several of them share.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"
RUNTIME=${1:?usage: $0 <rt> [case ...]}
shift
need_udid "$0" "$RUNTIME" UDID short

# ---------------------------------------------------------------- the cases
# A mutation-L<nn> case runs with the fix for bug L<nn> (its id in docs/ledger.md) switched off by that fix's
# SIMCAM_OFF_* switch, and passes if the case catches the bug coming back.
typeset -a CASES MOTION_CASES
CASES=("$@")
MOTION_CASES=(
  orient-nomotion-marker camera-then-motion-real motion-after-gate motion-latched motion-pf-expiry motion-crash
  mutation-L58 mutation-L59 mutation-L60 orient-crash mutation-L61 pf-prune motion-stock-threads
  motion-attitude motion-attitude-stock orient-crash-main mutation-L62 orient-crash-ensure mutation-L63 pf-prune-race
  mutation-L64 app-send-crash mutation-L65 pf-prune-kept
)

typeset -a TRACE_STOP_CASES
TRACE_STOP_CASES=(
  trace-crash-start mutation-L69 trace-crash2 mutation-L70 trace-repick trace-unwritable mutation-L71 flush-race
  mutation-L73 late-waiter mutation-L74
)

if (( ! ${#CASES} )); then
  CASES=(act-slow-ok act-slow-timeout act-fail)
  [[ $RUNTIME != 175 ]] && CASES+=(
    act-latch act-cli-clear act-latch-phase act-decay act-marker-multi act-swapback act-sentinel-off act-trim-race
    act-swap-slow act-start-slow act-control-slow act-override act-pause act-threads-pause act-timeout-throw
    act-swapthrow act-crash-prepare act-crash-fig act-handler-restore act-fake-sdk act-armed-crash act-never act-late
    act-got act-main act-reenter threads threads-slow orient-nomotion orient-sameturn orient-bg orient-bg-turnback
    motion-only motion-threads none motion-gate camera-then-motion mutation-L56 mutation-L57 act-traced
  )
  [[ $RUNTIME == 175 ]] && CASES+=(act-markers-175 act-175-record)
  [[ $RUNTIME != 175 ]] && CASES+=($MOTION_CASES $TRACE_STOP_CASES)
  if [[ ${MOTION_SUITE:-0} == 1 ]]; then
    CASES=(motion-only motion-threads none motion-gate camera-then-motion orient-nomotion $MOTION_CASES)
  fi
fi

# ---------------------------------------------------------------- run state
TAG=act-$(date +%H%M%S)
RESULTS_FILE=$SIMCAM_RESULTS/activation-$RUNTIME-$TAG.txt
: > $RESULTS_FILE
STATE_DIR=$SIMCAM_HOME/sims/$UDID/state
BUILD=$(<$ROOT/build/current_dylib.txt)
BUILD=${BUILD:h:t}

# the test apps' bundle ids: plain (fixtures/plain), the canary, plain-orient, the motion probe. m10, m12 and e24 are
# the milestone or experiment that added each app; bundle ids are data and keep those names.
PLAIN_BUNDLE=dev.simcam.spike.m10plain
CANARY_BUNDLE=dev.simcam.spike.m10canary
ORIENT_BUNDLE=dev.simcam.spike.m12plain
MOTION_BUNDLE=dev.simcam.e24.motionprobe

# the timing cases: a FAIL is run once more before it counts
TIMING_CASES=(
  act-slow-ok act-slow-timeout act-swap-slow act-start-slow act-override act-pause act-threads-pause act-main
  act-reenter threads-slow motion-threads act-never act-late
)

fails=0
unsures=0
VERDICT=
EVIDENCE=

# the cases (c_<case> functions) and their helpers, one file per concern
source "$ROOT/scripts/activation/helpers.sh"
source "$ROOT/scripts/activation/timing.sh"
source "$ROOT/scripts/activation/latch.sh"
source "$ROOT/scripts/activation/failures.sh"
source "$ROOT/scripts/activation/threads.sh"
source "$ROOT/scripts/activation/orientation.sh"
source "$ROOT/scripts/activation/motion.sh"
source "$ROOT/scripts/activation/pruning.sh"
source "$ROOT/scripts/activation/trace.sh"
trap unblock_state EXIT

# ---------------------------------------------------------------- driver
for case_name in $CASES; do
  case_fn=c_$case_name
  if (( ! $+functions[$case_fn] )); then
    echo "FAIL $case_name — unknown case" | tee -a $RESULTS_FILE
    fails=$((fails + 1))
    continue
  fi
  started_s=$SECONDS
  CASE_START=$(date +%s)
  $case_fn

  first_verdict=$VERDICT
  first_evidence=$EVIDENCE

  # ${TIMING_CASES[(Ie)$case_name]} is the case's index in TIMING_CASES, 0 if it isn't a timing case.
  if [[ $first_verdict == FAIL && ${TIMING_CASES[(Ie)$case_name]} -gt 0 ]]; then
    load_avg=$(sysctl -n vm.loadavg | tr -d '{}')
    CASE_START=$(date +%s)
    $case_fn
    line="$VERDICT $case_name — rerun after a FAIL (load avg$load_avg): first: $first_evidence | rerun: $EVIDENCE"
  else
    line="$first_verdict $case_name — $first_evidence"
  fi

  [[ ${line%% *} == FAIL ]] && fails=$((fails + 1))
  [[ ${line%% *} == UNSURE ]] && unsures=$((unsures + 1))
  echo "$line ($(( SECONDS - started_s )) s)" | tee -a $RESULTS_FILE
done

if (( fails )); then
  echo "$fails case(s) failed, $unsures UNSURE (results: $RESULTS_FILE)"
  exit 1
fi
if (( unsures )); then
  echo "$(( ${#CASES} - unsures )) of ${#CASES} cases passed, $unsures UNSURE, none failed (results: $RESULTS_FILE)"
else
  echo "all ${#CASES} cases passed (results: $RESULTS_FILE)"
fi
