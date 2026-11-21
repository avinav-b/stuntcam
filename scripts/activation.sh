#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# activation.sh <rt> [case ...] — bounded lazy activation and CoreMotion's load-time decision
#   (design §4.1, "Activation"), on a sim armed by
#   scripts/boot.sh <rt> stub. Prints "PASS <case> — evidence" / "FAIL <case> — evidence" (one line per case, also appended
#   to results/activation-<rt>-<tag>.txt) and exits 1 if any case failed. On 17.5: act-slow-ok act-slow-timeout act-fail + the -175 cases.
#   A timing case that fails is run once more before it counts (another agent's load can starve the machine); both
#   verdicts and the load average are printed.
#   THREADS_N (default 100): launches of the `threads` case. MOTION_N (default 10): launches of `motion-only`.
# Every counted case starts and ends with a cleared sentinel. The clear is done host-side (a clear line appended to the app's
# events.log and its latched-* markers removed, in the state dir and the container-tmp fallback): the same state a
# SIMCAM_SENTINEL=clear launch leaves, without its ~3-15 s launch.
#   cases (default: all that apply to <rt>, the list below; except on 17.5 it includes MOTION_CASES (CoreMotion at load,
#   tracker faults, marker pruning, attitude stand-in, app-answerable sends; MOTION_SUITE=1 runs only these and the motion cases)
#   and TRACE_STOP_CASES (op trace file, deferred-stop flush, crash-handler race, late stop waiter; they read traces with
#   cli/bin/stuntcam, build it first)).
# Verdicts: PASS, FAIL, and UNSURE (not counted as a failure; the run's last line counts them). Crash
# reports: a case that wants the host's .ips for a crash it caused matches reports by pid, never by count or mtime. The pids come
# from the launch's own lines: the fault line "[simcam <pid>] SIMCAM_TEST_*: fault ..." where the case has one, else the launch's
# first "[simcam <pid>]" line. A report counts if its process name is the app's and its "pid" is one of those, in
# DiagnosticReports or Retired/ (tools/ipspid.py), captured at or after the case's start (CASE_START: a reused pid never
# matches an older report); a missing one is waited for up to 30 s (reports were seen 12 s late). If
# every engine-side check passes and a pid still has no report, the case is UNSURE ("no crash report written for pid ..."):
# macOS stops writing reports for a process after many crashes, so a missing report is host state, not an engine verdict
# (seen: whenever m5app crashes got no report, DiagnosticReports held exactly 25 m5app reports, the rest in Retired/;
# once orient-crash-ensure's second report was missing with only 5 there, cause unknown). A case that wants NO report
# (act-slow-timeout) counts this launch's pid only.
#   act-traced attaches lldb from the command line (SB API, scripts/lldb-attach.py; no GUI). Not here: act-tsan (a TSan app +
#   engine boot)
# Layout: this file holds the case lists and the driver; each case <c> is the function c_<c> in scripts/activation/, one
# file per concern (helpers.sh holds what several of them share).
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh
RT=${1:?usage: $0 <rt> [case ...]}; shift
need_udid "$0" "$RT" U short
typeset -a CASES MOTION_CASES; CASES=("$@")
MOTION_CASES=(orient-nomotion-marker camera-then-motion-real motion-after-gate motion-latched motion-pf-expiry motion-crash mutation-L58 mutation-L59 mutation-L60
  orient-crash mutation-L61 pf-prune motion-stock-threads
  motion-attitude motion-attitude-stock orient-crash-main mutation-L62 orient-crash-ensure mutation-L63 pf-prune-race mutation-L64
  app-send-crash mutation-L65 pf-prune-kept)
typeset -a TRACE_STOP_CASES
TRACE_STOP_CASES=(trace-crash-start mutation-L69 trace-crash2 mutation-L70 trace-repick trace-unwritable mutation-L71 flush-race mutation-L73 late-waiter mutation-L74)
if (( ! ${#CASES} )); then
  CASES=(act-slow-ok act-slow-timeout act-fail)
  [[ $RT != 175 ]] && CASES+=(act-latch act-cli-clear act-latch-phase act-decay act-marker-multi act-swapback act-sentinel-off act-trim-race act-swap-slow
    act-start-slow act-control-slow act-override act-pause act-threads-pause act-timeout-throw act-swapthrow act-crash-prepare act-crash-fig act-handler-restore
    act-fake-sdk act-armed-crash act-never act-late act-got act-main act-reenter threads threads-slow orient-nomotion orient-sameturn orient-bg orient-bg-turnback
    motion-only motion-threads none motion-gate camera-then-motion mutation-L56 mutation-L57 act-traced)
  [[ $RT == 175 ]] && CASES+=(act-markers-175 act-175-record)
  [[ $RT != 175 ]] && CASES+=($MOTION_CASES $TRACE_STOP_CASES)
  [[ ${MOTION_SUITE:-0} == 1 ]] && CASES=(motion-only motion-threads none motion-gate camera-then-motion orient-nomotion $MOTION_CASES)
fi
TAG=act-$(date +%H%M%S); OUTF=$SIMCAM_RESULTS/activation-$RT-$TAG.txt; : > $OUTF
STATE=$SIMCAM_HOME/sims/$U/state; BUILD=$(<$ROOT/build/current_dylib.txt); BUILD=${BUILD:h:t}
BP=dev.simcam.spike.m10plain BC=dev.simcam.spike.m10canary BO=dev.simcam.spike.m12plain BM=dev.simcam.e24.motionprobe
TIMING=(act-slow-ok act-slow-timeout act-swap-slow act-start-slow act-override act-pause act-threads-pause act-main act-reenter
  threads-slow motion-threads act-never act-late)
fails=0; unsures=0; VERDICT=; EVID=
# the cases (c_<case> functions) and their helpers, one file per concern
source $ROOT/scripts/activation/helpers.sh
source $ROOT/scripts/activation/timing.sh
source $ROOT/scripts/activation/latch.sh
source $ROOT/scripts/activation/failures.sh
source $ROOT/scripts/activation/threads.sh
source $ROOT/scripts/activation/orientation.sh
source $ROOT/scripts/activation/motion.sh
source $ROOT/scripts/activation/pruning.sh
source $ROOT/scripts/activation/trace.sh
trap unblock_state EXIT

# ---------------------------------------------------------------- driver
for c in $CASES; do
  fn=c_$c; (( $+functions[$fn] )) || { echo "FAIL $c — unknown case" | tee -a $OUTF; fails=$((fails + 1)); continue; }
  t0=$SECONDS; CASE_START=$(date +%s); $fn; v1=$VERDICT; e1=$EVID
  if [[ $v1 == FAIL && ${TIMING[(Ie)$c]} -gt 0 ]]; then
    la=$(sysctl -n vm.loadavg | tr -d '{}'); CASE_START=$(date +%s); $fn
    line="$VERDICT $c — rerun after a FAIL (load avg$la): first: $e1 | rerun: $EVID"
  else line="$v1 $c — $e1"; fi
  [[ ${line%% *} == FAIL ]] && fails=$((fails + 1)); [[ ${line%% *} == UNSURE ]] && unsures=$((unsures + 1))
  echo "$line ($(( SECONDS - t0 )) s)" | tee -a $OUTF
done
(( fails )) && { echo "$fails case(s) failed, $unsures UNSURE (results: $OUTF)"; exit 1; }
(( unsures )) &&
  echo "$(( ${#CASES} - unsures )) of ${#CASES} cases passed, $unsures UNSURE, none failed (results: $OUTF)" ||
  echo "all ${#CASES} cases passed (results: $OUTF)"
