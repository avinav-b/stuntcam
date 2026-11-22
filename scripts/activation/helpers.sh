# SPDX-License-Identifier: Apache-2.0
# activation/helpers.sh — shared helpers: verdicts, launches, log counts, sentinel state,
# crash reports, plain-orient runs.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

# ---------------------------------------------------------------- shared numbers
PLAIN_CHECKS=14      # plain's checks in a normal armed run: "14/14" is every one of them passing
MARKER_LIFE_S=86400  # a latched or preflight-fail marker is good for 24 h
LATCH_WINDOW_ACTIVATIONS=20  # the engine's kSentinelWindowActivations: strikes count over the last 20 activations
ORDER_RUN_CHECKS=$(( PLAIN_CHECKS + 1 ))  # with M5APP_ORDER=<order>, plain adds one "order.<order>" check
PLAIN_PROCESS=m5app  # fixtures/plain's executable, as crash reports and the host's process list name it
CRASH_REPORT_WAIT_S=30  # how long crash_reports waits for a report the host hasn't written yet

# ---------------------------------------------------------------- verdicts (the driver prints VERDICT and EVIDENCE)
# ok/bad/unsure <text...>: set the case's verdict (PASS, FAIL or UNSURE) and its evidence, the arguments joined with
# one space
ok() {
  VERDICT=PASS
  EVIDENCE="$*"
}

bad() {
  VERDICT=FAIL
  EVIDENCE="$*"
}

unsure() {
  VERDICT=UNSURE
  EVIDENCE="$*"
}

# ---------------------------------------------------------------- launches and their run dirs
# run_dir <app> <n>: the result dir of `run <app> <n>` (run.sh <app> <rt> boot <tag>-<n>)
run_dir() { print -r -- "$SIMCAM_RESULTS/$1-$RUNTIME-boot-$TAG-$2"; }

# run <app> <n> [K=V ...]: one run.sh launch in boot mode, the K=V pairs injected for it; no retry, output dropped.
# XENV gets the pairs one per line ((F) joins with newlines), and the final newline tells run.sh to split at newlines
# only, so a value may hold spaces.
run() {
  local app=$1
  local n=$2
  shift 2
  # its output stays in its result dir; the verdict is read from there
  XENV="${(F)@}"$'\n' SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh $app $RUNTIME boot $TAG-$n >/dev/null 2>&1
}

# bg_run <app> <n> [K=V ...]: `run` in the background (XENV as in run); sets BG_PID (its pid) and BG_DIR (its run dir)
bg_run() {
  local app=$1
  local n=$2
  shift 2
  # its output stays in its result dir; the verdict is read from there
  ( XENV="${(F)@}"$'\n' SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh $app $RUNTIME boot $TAG-$n >/dev/null 2>&1 ) &
  BG_PID=$!
  BG_DIR=$(run_dir $app $n)
}

# run_plain_orient <mode> <orient> <tag> [K=V ...]: one plain-orient launch in boot mode, through p12.sh with the back
# camera. Sets LAST_RUN_DIR to its run dir.
run_plain_orient() {
  local mode=$1
  local orient=$2
  local tag=$3
  shift 3
  # MAXW is how many times p12.sh polls for the app's DONE, every 0.2 s: 150 polls make 30 s
  NOINJ=1 MAXW=150 $ROOT/scripts/p12.sh $RUNTIME $mode back $orient $TAG-$tag "$@" >/dev/null 2>&1
  LAST_RUN_DIR=$SIMCAM_RESULTS/p12-$mode-$RUNTIME-back-$orient-$TAG-$tag

  # p12.sh copies the newest engine log; copy this launch's own log (named by its pid) instead
  local pid=$(app_pid $LAST_RUN_DIR/app.log)
  local boot_logs=$(cat $ROOT/build/bootlogdir-$RUNTIME.txt)
  [[ -n $pid && -f $boot_logs/dylib-$pid.log ]] && cp $boot_logs/dylib-$pid.log $LAST_RUN_DIR/dylib.log
}

# ---------------------------------------------------------------- reading logs (each prints one value)
# count_matches <extended regex> <file>: how many lines of the file match (0 if the file is missing)
count_matches() {
  local count
  count=$(grep -cE "$1" $2 2>/dev/null)   # a missing file counts 0 lines
  print -r -- ${count:-0}
}

# pass_count <run dir>: how many "| PASS" lines the app logged
pass_count() { count_matches '\| PASS' $1/app.log; }

# line_count <file>: its number of lines (0 if missing)
line_count() { [[ -f $1 ]] && wc -l < $1 | tr -d ' ' || echo 0; }

# lines_after <file> <n>: the file's lines after its first <n> (nothing if missing)
lines_after() { [[ -f $1 ]] && tail -n +$(( $2 + 1 )) $1 || true; }

# app_lines <regex> <run dir> <width>: the app's log lines matching <regex> (grep -E) without their "M5APP| " prefix,
# joined with spaces and cut to <width> characters (a FAIL line's evidence)
app_lines() { grep -E "$1" $2/app.log | sed 's/M5APP| //' | tr '\n' ' ' | cut -c1-$3; }

# first_call_ms <run dir>: the app's first "firstcall.ms=" value (empty if none)
first_call_ms() { grep -oE 'firstcall\.ms=[0-9]+' $1/app.log 2>/dev/null | head -1 | cut -d= -f2; }

# later_call_ms <run dir>: the app's "latercall.ms=" values, one per line (empty if none)
later_call_ms() { grep -oE 'latercall\.ms=[0-9.]+' $1/app.log | cut -d= -f2; }

# first_line_number <extended regex> <file>: the line number of the file's first matching line (empty if none)
first_line_number() { grep -nE "$1" $2 | head -1 | cut -d: -f1; }

# first_cf_time <extended regex> <dylib.log>: the CFAbsoluteTime stamp (first field) of the first matching line
first_cf_time() { grep -m1 -E "$1" $2 2>/dev/null | awk '{print $1}'; }

# app_pid <app.log>: the pid in the launch's first "[simcam <pid>]" line
app_pid() { grep -oE '\[simcam [0-9]+\]' $1 2>/dev/null | head -1 | grep -oE '[0-9]+'; }

# orient_angles <run dir>: the preview and capture angles plus each photo/movie connection angle, on one line
orient_angles() {
  {
    grep -oE 'previewAngle=[0-9.]+ captureAngle=[0-9.]+' $1/app.log | head -1
    grep -oE '(photo|movie) connection angle=[0-9.]+' $1/app.log
  } | tr '\n' ' '
}

# in_range <value> <low> <high>: true if the value is set and low <= value <= high
in_range() { [[ -n $1 ]] && (( $1 >= $2 && $1 <= $3 )); }

# ---------------------------------------------------------------- CoreMotion verdicts from one run dir
# motion_stock_lines <run dir>: how many one-shot "CoreMotion is stock" lines the engine wrote to the launch's own
# log (the engine's record queue writes them to the per-pid log)
motion_stock_lines() { count_matches 'simcam: CoreMotion is stock this launch' $1/dylib.log; }

# motion_standin_answered <run dir> <order>: true if the stand-in answered the order's three checks (available, rate,
# timestamps), all 16 motion hooks went in, and no stock line was written
motion_standin_answered() {
  local answered=$(count_matches "PASS order.$2.(available|rate|timestamps)" $1/app.log)
  local hooked=$(count_matches 'motion: hooked 16' $1/app.log)
  [[ $answered == 3 && $hooked == 1 && $(motion_stock_lines $1) == 0 ]]
}

# motion_stock_once <run dir> <order>: true if CoreMotion stayed stock (both stock checks passed, no hooks) and the
# engine wrote exactly one stock line
motion_stock_once() {
  local stock=$(count_matches "PASS order.$2.stock.(available|updates)" $1/app.log)
  local hooked=$(count_matches 'motion: hooked' $1/app.log)
  [[ $stock == 2 && $hooked == 0 && $(motion_stock_lines $1) == 1 ]]
}

# ---------------------------------------------------------------- sentinel state on the host
# events_log <bundle>: the path of the bundle's events.log in the state dir
events_log() { print -r -- "$STATE_DIR/$1/events.log"; }

# fallback_state_dir <bundle>: the container-tmp fallback state dir of the bundle (a path that never exists if the app
# has no data container)
fallback_state_dir() {
  local container=$(xcrun simctl get_app_container $UDID $1 data 2>/dev/null)   # no container: an error, handled below
  [[ $container == $HOME/* ]] && print -r -- "$container/tmp/simcam-state/$1" || print -r -- "/nonexistent-container/$1"
}

# crash_counts <events.log> <n>: the crash lines after the first <n>, counted per kind and feature and sorted, as
# "<kind>-<feature>x<count> ..." (a trailing space after each)
crash_counts() {
  lines_after $1 $2 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' '
}

# camera_crashes_only <crash_counts output>: true if the two crashes counted against the camera (provider or
# activate), never motion
camera_crashes_only() { [[ $1 == (crash-providerx2|crash-activatex2|crash-activatex1 crash-providerx1)' ' ]]; }

# first_event_time <events.log> <n> <kind> [<feature>]: the epoch time (first field) of the first line after the
# first <n> whose kind (third field) is <kind> and, if given, whose feature (fourth field) is <feature>
first_event_time() {
  lines_after $1 $2 |
    awk -v kind="$3" -v feature="${4:-}" '$3 == kind && (feature == "" || $4 == feature) {print $1; exit}'
}

# max_activation_number <events.log>: the highest activation number (second field) in the file (0 if missing)
max_activation_number() { [[ -f $1 ]] && awk 'NF>=5 && $2>m {m=$2} END {print m+0}' $1 || echo 0; }

# count_existing <paths with (N) globs>: how many of the paths exist (a (N) glob that matches nothing adds none)
count_existing() { print $#; }

# mtime <file>: its modification time, epoch seconds (0 if missing)
mtime() { stat -f %m $1 2>/dev/null || echo 0; }

# setmtime <file> <epoch>: sets its access and modification times
setmtime() {
  python3 -I -c '
import os, sys
epoch = int(sys.argv[2])
os.utime(sys.argv[1], (epoch, epoch))' $1 $2
}

# clear_sentinel <bundle>: the host-side clear of one bundle's sentinel state (see activation.sh's header), in the state
# dir and the container-tmp fallback
clear_sentinel() {
  local bundle=$1
  local dir marker
  for dir in $STATE_DIR/$bundle "$(fallback_state_dir $bundle)"; do
    [[ -d $dir ]] || continue
    print -r -- "$(date +%s) 0 clear - $BUILD host-clear" >> $dir/events.log
    for marker in $dir/latched-*(N); do
      rm -f $marker
    done
    rm -f ${dir:h}/preflight-fail-$BUILD
  done
  rm -f $STATE_DIR/preflight-fail-$BUILD
}

# plant_marker <path> <content> <until epoch>: a marker the engine would have written (content, mtime = until)
plant_marker() {
  mkdir -p ${1:h}
  print -r -- "$2" > $1
  setmtime $1 $3
}

# plant_event <bundle> <line>: appends a line to the bundle's events.log
plant_event() { print -r -- "$2" >> "$(events_log $1)"; }

# 17.5 cases: the container-tmp fallback is forced by a regular file where the app's state directory would be (mkdir -p
# "succeeds" on it, events.log can't open under it). A per-launch SIMCAM_STATE_DIR doesn't work in boot mode: the boot
# environment's value wins.
# block_state: moves the plain app's state dir aside and puts that regular file in its place
block_state() {
  local dir=$STATE_DIR/$PLAIN_BUNDLE
  [[ -d $dir ]] && mv $dir $dir.w4-blocked
  [[ -e $dir ]] || print 'W4 activation.sh: blocks this state dir' > $dir
}

# unblock_state: undoes block_state (also the driver's EXIT trap); always succeeds
unblock_state() {
  local dir=$STATE_DIR/$PLAIN_BUNDLE
  [[ -f $dir ]] && rm -f $dir
  [[ -d $dir.w4-blocked && ! -e $dir ]] && mv $dir.w4-blocked $dir
  return 0
}

# ---------------------------------------------------------------- waiting
# wait_for <file> <extended regex> <seconds>: polls every 0.05 s until a line matches; false on timeout
# (common.sh's wait_for_line takes a basic regex, a count of 0.1 s polls and the launch's pid instead)
wait_for() {
  local i
  for i in {1..$(( $3 * 20 ))}; do
    grep -qE "$2" $1 2>/dev/null && return 0   # the file may not exist yet
    sleep 0.05
  done
  return 1
}

# ---------------------------------------------------------------- crash reports, matched by pid (see the header)
# fault_pid <app.log> [regex]: the pid of the launch's fault line (regex after "[simcam <pid>] "), or of its first
# [simcam <pid>] line with no regex
fault_pid() {
  # no app.log: no pid
  grep -m1 -oE "\[simcam [0-9]+\] ${2:-}" $1 2>/dev/null | grep -m1 -oE '^\[simcam [0-9]+' | grep -oE '[0-9]+'
}

# crash_reports <proc> <wait s> <pid> ...: finds the crash reports of process <proc> for those pids, captured since
# CASE_START (the driver sets it before each case run). It waits up to <wait s> seconds for missing ones. Empty pid
# arguments are skipped: a launch whose pid wasn't found passes "".
# A zsh function can't return values, so it sets these three globals, which verdict_reports and report_of read:
#   REPORTS_FOUND    how many of the pids have a report
#   PIDS_NO_REPORT   the pids without one, each after a space (verdict_reports prints it right after "pid(s)")
#   REPORT_FILE_OF   each pid's report file, by pid (an associative array)
crash_reports() {
  local proc=$1
  local wait_s=$2
  shift 2

  REPORTS_FOUND=0
  PIDS_NO_REPORT=
  typeset -gA REPORT_FILE_OF=()

  local -a pids
  local pid
  for pid in "$@"; do
    if [[ -n $pid ]]; then
      pids+=($pid)
    fi
  done
  (( ${#pids} )) || return 0

  local listing=$(report_listing $proc $wait_s $pids)
  local report
  for pid in $pids; do
    report=$(report_path "$listing" $pid)
    if [[ -n $report ]]; then
      REPORTS_FOUND=$(( REPORTS_FOUND + 1 ))
      REPORT_FILE_OF[$pid]=$report
    else
      PIDS_NO_REPORT="$PIDS_NO_REPORT $pid"
    fi
  done
}

# report_listing <proc> <wait s> <pid> ...: ipspid.py's listing of those pids' reports ("<pid><TAB><path>" lines),
# asked once a second until every pid has one or <wait s> seconds have passed
report_listing() {
  local proc=$1
  local wait_s=$2
  shift 2
  local try listing

  for try in {0..$wait_s}; do
    listing=$(python3 -I "$ROOT/tools/ipspid.py" $proc ${CASE_START:-0} "$@")
    if (( $(distinct_report_pids "$listing") >= $# || try == wait_s )); then
      break
    fi
    sleep 1
  done
  print -r -- "$listing"
}

# distinct_report_pids <ipspid.py output>: how many different pids have a report in it
distinct_report_pids() { print -r -- "$1" | awk -F'\t' 'NF {print $1}' | sort -u | grep -c .; }

# report_path <ipspid.py output> <pid>: the first report file listed for that pid (empty if none)
report_path() { print -r -- "$1" | awk -F'\t' -v k=$2 '$1 == k {print $2; exit}'; }

# report_of <pid>: that pid's report file (after crash_reports)
report_of() {
  if (( ${+REPORT_FILE_OF[$1]} )); then
    print -r -- "${REPORT_FILE_OF[$1]}"
  fi
}

# verdict_reports <engine ok 0|1> <want> <ok text> <bad text>: PASS if the engine side passed and all <want> pids have a
# report; UNSURE if it passed and some have none; FAIL otherwise
verdict_reports() {
  local engine_ok=$1
  local want=$2
  if (( engine_ok )) && (( REPORTS_FOUND == want )); then
    ok "$3"
  elif (( engine_ok )) && (( REPORTS_FOUND < want )) && [[ -n $PIDS_NO_REPORT ]]; then
    unsure "no crash report written for pid(s)$PIDS_NO_REPORT (DiagnosticReports + Retired, waited 30 s;" \
      "$REPORTS_FOUND of $want found: host report cap?); engine side passed: $3"
  else
    bad "$4"
  fi
}

# ips_fault <file>: "<exception type> queue=<faulting thread's queue> top=<image of the top frame>"
ips_fault() {
  python3 -I - $1 <<'PY'
import json, sys

# an .ips file is a one-line JSON header, then the report's JSON body
header_and_body = open(sys.argv[1], errors='replace').read().split('\n', 1)
report = json.loads(header_and_body[1])
threads = report.get('threads') or []
faulting_index = report.get('faultingThread', 0)
faulting_thread = threads[faulting_index] if 0 <= faulting_index < len(threads) else {}
images = report.get('usedImages') or []
top_frame = (faulting_thread.get('frames') or [{}])[0]
image_index = top_frame.get('imageIndex')
image_known = isinstance(image_index, int) and image_index < len(images)
top_image = images[image_index].get('name') if image_known else '?'
exception_type = (report.get('exception') or {}).get('type')
print(exception_type, 'queue=%s' % faulting_thread.get('queue'), 'top=%s' % top_image)
PY
}
