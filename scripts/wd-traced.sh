#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# wd-traced.sh <rt> [reps] — the watchdog's debugger rule (safety.inc SCWatchdog: "while P_TRACED, an expired op is
#   recorded as unknown-hang and never counted"), on a sim armed by scripts/boot.sh <rt> stub, with fixtures/plain.
#   traced   x reps: plain with SIMCAM_WD_TEST=hang, launched with simctl --wait-for-debugger; scripts/lldb-attach.py
#            attaches by pid, continues, stays attached and running (no breakpoints, the watchdog keeps ticking) until
#            the engine log has both 'over its deadline while a debugger is attached' lines, then detaches and quits;
#            the app runs on to DONE untraced. Expect: 2 'unknown-hang photo', 0 'hang', 'debugger detached', no latch,
#            no latched-* marker. Then a launch with no debugger and no test hook: armed, 14/14, no latch.
#   control  x reps: the same hang launch with no debugger: 2 'hang photo' (n=1, n=2) and an in-process latch; the next
#            launch is latched with a latched-<build> marker. A SIMCAM_SENTINEL=clear launch after each rep.
#   Prints "PASS|FAIL <step> — evidence"; exits 1 if any step failed.
#   Results: $SIMCAM_RESULTS/plain-<rt>-boot-<tag>-<step>/
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

RUNTIME=${1:?usage: $0 <rt> [reps]}
REPS=${2:-3}
need_udid "$0" "$RUNTIME" UDID short

# fixtures/plain's bundle id and product (m10, m5: the milestones that added it; names that are data stay)
PLAIN_BUNDLE=dev.simcam.spike.m10plain
PLAIN_EXECUTABLE=m5app.app/m5app   # fixtures/plain's product name
STATE_DIR=$SIMCAM_HOME/sims/$UDID/state/$PLAIN_BUNDLE
EVENTS=$STATE_DIR/events.log

BUILD=$(<"$ROOT/build/current_dylib.txt")
BUILD=${BUILD:h:t}

XCODE_PYTHON=/Applications/Xcode.app/Contents/Developer/usr/bin/python3
LLDB_PYTHONPATH=$(xcrun lldb -P)
TAG=wdt-$(date +%H%M%S)
fails=0

# What a passing run shows.
PLAIN_CHECKS=14         # fixtures/plain's checks: the "| PASS" lines of a full run
HANG_EXPIRIES=2         # SIMCAM_WD_TEST=hang expires the photo op twice; untraced, the second expiry latches photo
PID_POLLS=200           # look for the app's pid for up to 10 s ...
PID_POLL_INTERVAL=0.05  # ... in steps of 0.05 s
ATTACH_TIMEOUT_S=40     # lldb-attach.py detaches after this long even if the log never matched

# run_dir <tag>: the result dir of `run <tag>`
run_dir() { print -r -- "$SIMCAM_RESULTS/plain-$RUNTIME-boot-$TAG-$1"; }

# run <tag> [K=V ...]: one plain launch in boot mode, the K=V pairs injected for it; no retry, output dropped.
# XENV gets the pairs one per line ((F) joins with newlines), and the final newline tells run.sh to split at newlines
# only, so a value may hold spaces.
run() {
  local tag=$1
  shift
  # its output stays in its result dir; the verdict is read from there
  XENV="${(F)@}"$'\n' SIMCAM_NO_RETRY=1 "$ROOT/scripts/run.sh" plain "$RUNTIME" boot "$TAG-$tag" >/dev/null 2>&1
}

# line_count <file>: its number of lines (0 if missing)
line_count() { [[ -f $1 ]] && wc -l < "$1" | tr -d ' ' || echo 0; }

# events_since <n>: the events.log lines after its first <n> (nothing if missing)
events_since() { [[ -f $EVENTS ]] && tail -n +$(( $1 + 1 )) "$EVENTS" || true; }

# count_matches <extended regex> <file>: how many lines of the file match (0 if the file is missing)
count_matches() {
  local n
  n=$(grep -cE "$1" "$2" 2>/dev/null)   # a missing file counts 0 lines
  print -r -- ${n:-0}
}

# fallback_state_dir: plain's container-tmp fallback state dir (a path that never exists without a data container)
fallback_state_dir() {
  local container=$(xcrun simctl get_app_container "$UDID" "$PLAIN_BUNDLE" data 2>/dev/null)   # none: see below
  [[ $container == $HOME/* ]] && print -r -- "$container/tmp/simcam-state/$PLAIN_BUNDLE" || print -r -- /nonexistent
}

# markers: plain's latched-* markers in the state dir and the fallback, one per line
markers() { print -l $STATE_DIR/latched-*(N) $(fallback_state_dir)/latched-*(N); }

# marker_count: how many latched-* markers plain has
marker_count() { markers | grep -c .; }

ok() { echo "PASS $1 — $2"; }
bad() {
  echo "FAIL $1 — $2"
  fails=$((fails + 1))
}
clear_engine() { run "$1" SIMCAM_SENTINEL=clear; }

# launch_and_record <tag> [K=V ...]: runs `run <tag>` and writes the events it added to <run dir>/events.txt
launch_and_record() {
  local events_before=$(line_count "$EVENTS")
  run "$@"
  events_since "$events_before" > "$(run_dir "$1")/events.txt"
}

# debugserver_left <pid>: "yes" if a debugserver still names the whole pid (no pid counts as left), else "no"
debugserver_left() {
  if [[ -z $1 ]]; then
    echo yes
    return
  fi
  pgrep -f "debugserver.*[^0-9]$1([^0-9]|\$)" >/dev/null && echo yes || echo no
}

# app_pid: the app's pid from the host's process list (its path names this sim's UDID), "" if it doesn't appear
app_pid() {
  local pid= attempt
  for attempt in {1..$PID_POLLS}; do
    pid=$(pgrep -f "Devices/$UDID/data/Containers/Bundle/Application/.*/$PLAIN_EXECUTABLE" | head -1)
    [[ -n $pid ]] && break
    sleep $PID_POLL_INTERVAL
  done
  print -r -- "$pid"
}

# The run tags of each repetition <rep> (the result dir is run_dir <tag>):
#   tr<rep> traced, nx<rep> the next launch, c<rep> control, cn<rep> control's next launch, cc<rep> the clear launch.

# traced_step <rep>: the hang launch with lldb attached and running through the whole activation
traced_step() {
  local rep=$1
  local tag=tr$rep
  local dir=$(run_dir "$tag")
  local attach_log=$SIMCAM_RESULTS/wd-traced-$RUNTIME-$TAG-attach$rep.txt
  local events_before=$(line_count "$EVENTS")
  # its output stays in its result dir; the verdict is read from there
  ( XENV="SIMCAM_WD_TEST=hang"$'\n' SIMCAM_NO_RETRY=1 WAIT_DEBUGGER=1 \
    "$ROOT/scripts/run.sh" plain "$RUNTIME" boot "$TAG-$tag" >/dev/null 2>&1 ) &
  local run_pid=$!

  local pid=$(app_pid)
  local engine_log="$(cat "$ROOT/build/bootlogdir-$RUNTIME.txt")/dylib-$pid.log"   # this pid's engine log (boot mode)
  if [[ -n $pid ]]; then
    # detach once the engine has logged both expired-op lines
    PYTHONPATH=$LLDB_PYTHONPATH "$XCODE_PYTHON" "$ROOT/scripts/lldb-attach.py" "$pid" "$engine_log" \
      '(?s)over its deadline while a debugger.*over its deadline while a debugger' $ATTACH_TIMEOUT_S \
      > "$attach_log" 2>&1
  else
    echo "ATTACH| no pid" > "$attach_log"
  fi

  wait $run_pid
  local left=$(debugserver_left "$pid")
  events_since "$events_before" > "$dir/events.txt"

  local unknown_hangs=$(count_matches ' unknown-hang photo ' "$dir/events.txt")
  local hangs=$(count_matches ' hang ' "$dir/events.txt")
  local traced_activations=$(count_matches ' activate .*traced=1' "$dir/events.txt")
  local debugger_lines=$(count_matches 'photo op over its deadline while a debugger is attached \(not counted\)' \
    "$dir/dylib.log")
  local latched_lines=$(count_matches 'latched' "$dir/dylib.log")
  local markers_left=$(marker_count)
  local passes=$(count_matches '\| PASS' "$dir/app.log")
  local attach_line=$(grep -m1 -oE 'attached pid [0-9]+ in [0-9.]+ s|attach FAILED.*|no pid' "$attach_log")
  local watchdog_detached=$(count_matches 'watchdog: debugger detached' "$dir/dylib.log")
  local lldb_detached=$(count_matches 'ATTACH\| .* detached' "$attach_log")

  local evidence
  if [[ $attach_line == attached* &&
        $traced_activations == 1 &&
        $unknown_hangs == "$HANG_EXPIRIES" &&
        $hangs == 0 &&
        $debugger_lines == "$HANG_EXPIRIES" &&
        $latched_lines == 0 &&
        $markers_left == 0 &&
        $passes == "$PLAIN_CHECKS" &&
        $left == no &&
        $watchdog_detached == 1 &&
        $lldb_detached == 1 ]]; then
    evidence="$attach_line, activate traced=1, $HANG_EXPIRIES unknown-hang photo, 0 hang, 0 latched lines, 0 markers,"
    evidence+=" lldb detached (watchdog saw it), $PLAIN_CHECKS/$PLAIN_CHECKS, no debugserver left"
    ok "traced-$rep" "$evidence"
  else
    evidence="attach='$attach_line' traced=1:$traced_activations unknown-hang=$unknown_hangs hang=$hangs"
    evidence+=" watchdog-lines=$debugger_lines latched=$latched_lines markers=$markers_left"
    evidence+=" pass=$passes lldb-detached=$lldb_detached watchdog-detached=$watchdog_detached"
    evidence+=" debugserver-left=$left (see $dir)"
    bad "traced-$rep" "$evidence"
  fi
}

# next_step <rep>: the launch after the traced one, with no debugger and no test hook, is still armed
next_step() {
  local rep=$1
  local tag=nx$rep
  local dir=$(run_dir "$tag")
  launch_and_record "$tag"

  local armed=$(count_matches 'LAZY armed by' "$dir/dylib.log")
  local latched_lines=$(count_matches 'latched' "$dir/dylib.log")
  local markers_left=$(marker_count)
  local passes=$(count_matches '\| PASS' "$dir/app.log")
  local untraced_activations=$(count_matches ' activate .*traced=0' "$dir/events.txt")
  local debug_run_lines=$(count_matches 'last debug run ended inside photo op' "$dir/dylib.log")

  local evidence
  if [[ $armed == 1 &&
        $latched_lines == 0 &&
        $markers_left == 0 &&
        $passes == "$PLAIN_CHECKS" &&
        $untraced_activations == 1 ]]; then
    evidence="armed (activate traced=0), $PLAIN_CHECKS/$PLAIN_CHECKS, no latch, 0 markers;"
    evidence+=" 'last debug run ended inside photo op': $debug_run_lines"
    ok "next-$rep" "$evidence"
  else
    evidence="armed=$armed latched=$latched_lines markers=$markers_left pass=$passes"
    evidence+=" traced=0:$untraced_activations (see $dir)"
    bad "next-$rep" "$evidence"
  fi
}

# control_step <rep>: the same hang launch with no debugger counts both hangs and latches photo in-process
control_step() {
  local rep=$1
  local tag=c$rep
  local dir=$(run_dir "$tag")
  launch_and_record "$tag" SIMCAM_WD_TEST=hang

  local hangs_n1=$(count_matches ' hang photo .* n=1' "$dir/events.txt")
  local hangs_n2=$(count_matches ' hang photo .* n=2' "$dir/events.txt")
  local unknown_hangs=$(count_matches 'unknown-hang' "$dir/events.txt")
  local in_process_latches=$(count_matches 'photo latched after 2 hangs' "$dir/dylib.log")

  local evidence
  if [[ $hangs_n1 == 1 && $hangs_n2 == 1 && $unknown_hangs == 0 && $in_process_latches == 1 ]]; then
    ok "control-$rep" "hang photo n=1 and n=2 counted, 0 unknown-hang, 'photo latched after 2 hangs' in-process"
  else
    evidence="hang n=1:$hangs_n1 n=2:$hangs_n2 unknown-hang=$unknown_hangs"
    evidence+=" in-process-latch=$in_process_latches (see $dir)"
    bad "control-$rep" "$evidence"
  fi
}

# control_next_step <rep>: the launch after the control one starts latched and leaves a latched-<build> marker
control_next_step() {
  local rep=$1
  local tag=cn$rep
  local dir=$(run_dir "$tag")
  launch_and_record "$tag"

  local marker_list=$(markers)
  local sentinel_latched=$(count_matches 'sentinel: photo latched' "$dir/dylib.log")
  local latched_activations=$(count_matches ' activate .*latched=1' "$dir/events.txt")
  local build_markers=$(print -r -- $marker_list | grep -c "latched-$BUILD")

  local evidence
  if [[ $sentinel_latched == 1 && $latched_activations == 1 && $build_markers == 1 ]]; then
    evidence="next launch latched: 'sentinel: photo latched', activate latched=1, marker ${marker_list##*/}"
    ok "control-next-$rep" "$evidence"
  else
    evidence="sentinel-latched=$sentinel_latched activate-latched=$latched_activations"
    evidence+=" markers='$marker_list' (see $dir)"
    bad "control-next-$rep" "$evidence"
  fi
  print -r -- "$marker_list" > "$dir/markers.txt"
}

# clear_step <rep>: a SIMCAM_SENTINEL=clear launch removes the marker
clear_step() {
  local rep=$1
  clear_engine "cc$rep"
  local markers_left=$(marker_count)
  if (( markers_left == 0 )); then
    ok "clear-$rep" "SIMCAM_SENTINEL=clear launch removed the marker"
  else
    bad "clear-$rep" "$markers_left markers left after the clear launch"
  fi
}

echo "wd-traced $RUNTIME: udid $UDID build $BUILD events $EVENTS tag $TAG"
[[ -e $HOME/.lldbinit-Xcode ]] && echo "note: ~/.lldbinit-Xcode exists before the run"

clear_engine clear0
echo "markers after the first clear: $(marker_count)"

for rep in {1..$REPS}; do
  traced_step "$rep"
  next_step "$rep"
done

for rep in {1..$REPS}; do
  control_step "$rep"
  control_next_step "$rep"
  clear_step "$rep"
done

if [[ -e $HOME/.lldbinit-Xcode ]]; then
  bad lldbinit "~/.lldbinit-Xcode exists"
else
  echo "no ~/.lldbinit-Xcode"
fi

if (( fails )); then
  echo "$fails step(s) failed"
  exit 1
fi
echo "all steps passed"
