# SPDX-License-Identifier: Apache-2.0
# activation/orientation.sh — the UI orientation snapshot and faults in the orientation tracker
# and other camera code.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.

c_orient-nomotion() {
  clear_sentinel $ORIENT_BUNDLE

  run_plain_orient orient landscapeLeft nm1 SIMCAM_OFF_MOTION=1
  local angles_off=$(orient_angles $LAST_RUN_DIR)
  local dir_off=$LAST_RUN_DIR
  run_plain_orient orient landscapeLeft nm0
  local angles_on=$(orient_angles $LAST_RUN_DIR)
  local hooked=$(count_matches 'motion: hooked' $dir_off/app.log)
  local done_lines=$(count_matches 'P12\| DONE' $dir_off/app.log)

  if [[ -n $angles_off && $angles_off == $angles_on && $angles_off != *captureAngle=90* && $hooked == 0 &&
    $done_lines == 1 ]]; then
    ok "SIMCAM_OFF_MOTION=1 (CoreMotion stock): '$angles_off', same as without the switch"
  else
    bad "no-motion '$angles_off' vs with motion '$angles_on' (hooked=$hooked)"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

c_orient-sameturn() {
  clear_sentinel $ORIENT_BUNDLE

  run_plain_orient coord-first landscapeLeft sf P12_UI=landscape
  local verdict=$(grep -m1 -E '(PASS|FAIL) coord-first' $LAST_RUN_DIR/app.log)

  if [[ $verdict == *PASS* ]]; then
    ok "${verdict#*\| } ($(grep -m1 'COORD same-turn' $LAST_RUN_DIR/app.log | grep -oE 'ui=[0-9]+ dev=[0-9]+'))"
  else
    bad "${verdict:-no verdict}; $(grep 'COORD' $LAST_RUN_DIR/app.log | tr '\n' ' ' | cut -c1-200)"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

# coord_lines <extended regex> <run dir>: plain-orient's matching log lines without their "P12| " prefix, each
# followed by ";", cut to 240 characters
coord_lines() { grep -E "$1" $2/app.log | sed 's/^P12| //' | tr '\n' ';' | cut -c1-240; }

c_orient-bg() {
  clear_sentinel $ORIENT_BUNDLE

  run_plain_orient coord-bg landscapeLeft cb P12_UI=landscape
  local verdict=$(grep -m1 -E '(PASS|FAIL) coord-bg' $LAST_RUN_DIR/app.log)
  local coord=$(coord_lines 'COORD' $LAST_RUN_DIR)
  local detail="$coord $(grep -m1 -oE 'ui orientation at load: .*' $LAST_RUN_DIR/dylib.log)"

  if [[ $verdict == *PASS* ]]; then
    ok "${verdict#*\| } — $detail"
  else
    bad "${verdict:-no verdict} — $detail"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

# The UI turns landscape, then back to portrait, before the first camera call. The snapshot must follow both turns.
c_orient-bg-turnback() {
  clear_sentinel $ORIENT_BUNDLE

  run_plain_orient coord-bg landscapeLeft tb P12_UI=turnback
  local verdict=$(grep -m1 -E '(PASS|FAIL) coord-bg-turnback' $LAST_RUN_DIR/app.log)
  local snap=$(grep -m1 -oE 'ui orientation at load: .*' $LAST_RUN_DIR/dylib.log)
  local coord=$(coord_lines 'COORD|turnback ui' $LAST_RUN_DIR)
  local detail="$coord $snap"

  if [[ $verdict == *PASS* && $snap == *'then 3 (geometry change), then 1 (geometry change)'* ]]; then
    ok "${verdict#*\| } — $detail"
  else
    bad "${verdict:-no verdict} — $detail"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

# Mutant SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1 takes no UI orientation snapshot at load. orient-bg must catch it.
c_mutation-L57() {
  clear_sentinel $ORIENT_BUNDLE

  run_plain_orient coord-bg landscapeLeft l57 P12_UI=landscape SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1
  local verdict=$(grep -m1 -E '(PASS|FAIL) coord-bg' $LAST_RUN_DIR/app.log)
  local off_rx='ui orientation at load: off \(SIMCAM_OFF_UI_ORIENTATION_AT_LOAD\)'
  local snapshot_off=$(count_matches "$off_rx" $LAST_RUN_DIR/dylib.log)

  if [[ $verdict == *FAIL*preview=90* && $snapshot_off == 1 ]]; then
    ok "mutant caught: ${verdict#*\| } (the coordinator on the camera queue reads portrait until main frees)"
  else
    bad "mutant not caught: ${verdict:-no verdict};" \
      "$(grep -m1 -oE 'ui orientation at load: .*' $LAST_RUN_DIR/dylib.log)"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

# orient_crash_seq <tag> [K=V ...]: plain-orient in coord-bg mode (armed, with a rotation coordinator). Launches N and
# N+1 fault in the tracker's upd; N+2 runs without the fault. Sets these globals:
#   ORIENT_CRASH_COUNTS   the crash lines of N and N+1, counted as crash_counts prints them
#   ORIENT_CRASH_PIDS     the pids of N's and N+1's fault lines, space-separated
#   ORIENT_CRASH_DIR3     the run dir of N+2
orient_crash_seq() {
  local tag=$1
  shift
  local events=$(events_log $ORIENT_BUNDLE)
  local events_seen
  clear_sentinel $ORIENT_BUNDLE
  events_seen=$(line_count $events)
  run_plain_orient coord-bg landscapeLeft $tag-1 P12_UI=landscape SIMCAM_TEST_ORIENT_FAULT=1 "$@"
  ORIENT_CRASH_PIDS=$(fault_pid $LAST_RUN_DIR/app.log 'SIMCAM_TEST_ORIENT_FAULT: fault')
  run_plain_orient coord-bg landscapeLeft $tag-2 P12_UI=landscape SIMCAM_TEST_ORIENT_FAULT=1 "$@"
  ORIENT_CRASH_PIDS="$ORIENT_CRASH_PIDS $(fault_pid $LAST_RUN_DIR/app.log 'SIMCAM_TEST_ORIENT_FAULT: fault')"
  ORIENT_CRASH_COUNTS=$(crash_counts $events $events_seen)
  run_plain_orient coord-bg landscapeLeft $tag-3 P12_UI=landscape "$@"
  ORIENT_CRASH_DIR3=$LAST_RUN_DIR
}

# A fault in the orientation tracker is camera code: it counts against the camera, never `motion`, so N+2 is
# camera-latched.
c_orient-crash() {
  orient_crash_seq oc
  local verdict3=$(grep -m1 -E '(PASS|FAIL) coord-bg' $ORIENT_CRASH_DIR3/app.log)
  local engine_ok=0
  local -a pids
  pids=(${=ORIENT_CRASH_PIDS})
  crash_reports p12 30 $pids
  local latched3=$(count_matches 'latched after' $ORIENT_CRASH_DIR3/dylib.log)

  if camera_crashes_only "$ORIENT_CRASH_COUNTS" && [[ ${#pids} == 2 && $latched3 == 1 &&
    $verdict3 == *'FAIL coord-bg no device'* ]]; then
    engine_ok=1
  fi

  local ok_text="N, N+1 (pids $ORIENT_CRASH_PIDS) faulted in upd after the coordinators' KVO:"
  ok_text+=" $ORIENT_CRASH_COUNTS(no motion),"
  ok_text+=" $REPORTS_FOUND crash reports; N+2 camera-latched in PREPARE 1 (${verdict3#*\| })"
  local bad_text="events '$ORIENT_CRASH_COUNTS' fault pids '$ORIENT_CRASH_PIDS' reports=$REPORTS_FOUND;"
  bad_text+=" N+2 latched=$latched3 '${verdict3:-no verdict}'"
  verdict_reports $engine_ok 2 "$ok_text" "$bad_text"
  clear_sentinel $ORIENT_BUNDLE
}

# Mutant SIMCAM_OFF_TRACKER_KEY_CLEAR=1 runs upd under the motion key again. orient-crash must catch it.
c_mutation-L61() {
  orient_crash_seq l61 SIMCAM_OFF_TRACKER_KEY_CLEAR=1
  local verdict3=$(grep -m1 -E '(PASS|FAIL) coord-bg' $ORIENT_CRASH_DIR3/app.log)
  local latched3=$(count_matches 'latched after' $ORIENT_CRASH_DIR3/dylib.log)

  if [[ $ORIENT_CRASH_COUNTS == 'crash-motionx2 ' && $latched3 == 0 ]]; then
    ok "mutant caught: '$ORIENT_CRASH_COUNTS', N+2 camera not latched" \
      "(${verdict3#*\| }; orient-crash wants provider and a latched camera)"
  else
    bad "mutant not caught: events '$ORIENT_CRASH_COUNTS'; N+2 latched=$latched3 '${verdict3:-no verdict}'"
  fi

  clear_sentinel $ORIENT_BUNDLE
}

# fault_text <run dir>: the app's first "SIMCAM_TEST_<switch>: fault in <function> now (<detail>)" line, from "fault
# in" on
fault_text() {
  local rx='SIMCAM_TEST_[A-Z_]+: fault in [A-Za-z0-9]+ now \([^)]*\)'
  grep -m1 -oE "$rx" $1/app.log | sed 's/^SIMCAM_TEST_[A-Z_]*: //'
}

# orient_early_seq <tag> <fault env N> <fault env N+1> <order> [K=V ...]: launches N and N+1 of plain (M5APP_ORDER=
# <order>) fault in camera code outside upd (SIMCAM_TEST_ORIENT_FAULT=<variant> or SIMCAM_TEST_TRAP_FAULT=1); N+2 is
# a plain camera-then-motion launch. Sets these globals:
#   EARLY_CRASH_COUNTS   the crash lines of N and N+1, counted as crash_counts prints them
#   EARLY_FAULTS         the fault lines of N and N+1, as "<N's>; <N+1's>"
#   EARLY_PIDS           the pids of those fault lines, space-separated
#   EARLY_DIR3           the run dir of N+2
# Only the cases that want the crash reports look them up (early_reports).
orient_early_seq() {
  local tag=$1
  local fault_env1=$2
  local fault_env2=$3
  local order=$4
  shift 4
  local events=$(events_log $PLAIN_BUNDLE)
  local events_seen
  clear_sentinel $PLAIN_BUNDLE
  events_seen=$(line_count $events)
  run plain $tag-1 M5APP_ORDER=$order $fault_env1 "$@"
  local dir1=$(run_dir plain $tag-1)
  run plain $tag-2 M5APP_ORDER=$order $fault_env2 "$@"
  local dir2=$(run_dir plain $tag-2)
  EARLY_CRASH_COUNTS=$(crash_counts $events $events_seen)
  local fault1=$(fault_text $dir1)
  local fault2=$(fault_text $dir2)
  EARLY_FAULTS="$fault1; $fault2"
  local pid1=$(fault_pid $dir1/app.log 'SIMCAM_TEST_[A-Z_]+: fault in')
  local pid2=$(fault_pid $dir2/app.log 'SIMCAM_TEST_[A-Z_]+: fault in')
  EARLY_PIDS="$pid1 $pid2"
  run plain $tag-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT=stock "$@"
  EARLY_DIR3=$(run_dir plain $tag-3)
}

# early_reports: runs crash_reports for EARLY_PIDS, which sets REPORTS_FOUND and PIDS_NO_REPORT. Also sets
# EARLY_PID_COUNT to the number of those pids.
early_reports() {
  local -a pids
  pids=(${=EARLY_PIDS})
  crash_reports $PLAIN_PROCESS $CRASH_REPORT_WAIT_S $pids
  EARLY_PID_COUNT=${#pids}
}

# early_verdict <want faults> <latch note>: the verdict of an orient_early_seq case; it runs early_reports. The
# engine side passed if N and N+1 crashed as camera code with the wanted fault lines and N+2 found no camera (it was
# latched). <latch note> goes after "camera-latched" in the PASS text.
early_verdict() {
  local want=$1
  local latch_note=$2
  local engine_ok=0
  local discovery=$(grep -oE 'camera-then-motion discovery=[0-9]+' $EARLY_DIR3/app.log)
  early_reports
  local latched3=$(count_matches 'latched after' $EARLY_DIR3/dylib.log)

  if camera_crashes_only "$EARLY_CRASH_COUNTS" && [[ $EARLY_PID_COUNT == 2 && $EARLY_FAULTS == "$want" &&
    $latched3 == 1 && $discovery == *=0 ]]; then
    engine_ok=1
  fi

  local ok_text="N, N+1 (pids $EARLY_PIDS): $EARLY_FAULTS; events $EARLY_CRASH_COUNTS(no motion),"
  ok_text+=" $REPORTS_FOUND crash reports;"
  ok_text+=" N+2 camera-latched$latch_note ($discovery)"
  local bad_text="faults '$EARLY_FAULTS' (pids '$EARLY_PIDS') events '$EARLY_CRASH_COUNTS' reports=$REPORTS_FOUND;"
  bad_text+=" N+2 latched=$latched3 $discovery"
  verdict_reports $engine_ok 2 "$ok_text" "$bad_text"
}

# The first motion call comes on main. Faults in OrientTrackStart (launch N) and in OrientTrackRunMain before upd
# (launch N+1) are camera code.
c_orient-crash-main() {
  orient_early_seq ocm SIMCAM_TEST_ORIENT_FAULT=start SIMCAM_TEST_ORIENT_FAULT=runmain motion-main-camera-bg
  early_verdict 'fault in OrientTrackStart now (main thread); fault in OrientTrackRunMain now (main thread)' \
    ' in PREPARE 1'
  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1 makes OrientTrackStart keep the motion key (it skips motion.inc's
# clear). orient-crash-main must catch it.
c_mutation-L62() {
  orient_early_seq l62 SIMCAM_TEST_ORIENT_FAULT=start SIMCAM_TEST_ORIENT_FAULT=runmain motion-main-camera-bg \
    SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1
  local latched3=$(count_matches 'latched after' $EARLY_DIR3/dylib.log)

  if [[ $EARLY_CRASH_COUNTS == 'crash-motionx2 ' && $latched3 == 0 ]]; then
    ok "mutant caught: '$EARLY_CRASH_COUNTS' ($EARLY_FAULTS), N+2 camera not latched" \
      "(orient-crash-main wants provider/activate and a latched camera)"
  else
    bad "mutant not caught: events '$EARLY_CRASH_COUNTS' ($EARLY_FAULTS); N+2 latched=$latched3"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# The first camera call arrives on main with the motion key set. ActEnsure's tracker run is camera code.
c_orient-crash-ensure() {
  orient_early_seq oce SIMCAM_TEST_ORIENT_FAULT=ensure SIMCAM_TEST_ORIENT_FAULT=ensure camera-then-motion
  early_verdict 'fault in OrientTrackRunMain now (main thread); fault in OrientTrackRunMain now (main thread)' ''
  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_ENSURE_KEY_CLEAR=1 makes ActEnsure's main-thread tracker run keep the key (it skips lazy.inc's
# clear). orient-crash-ensure must catch it.
c_mutation-L63() {
  orient_early_seq l63 SIMCAM_TEST_ORIENT_FAULT=ensure SIMCAM_TEST_ORIENT_FAULT=ensure camera-then-motion \
    SIMCAM_OFF_ENSURE_KEY_CLEAR=1
  local latched3=$(count_matches 'latched after' $EARLY_DIR3/dylib.log)

  if [[ $EARLY_CRASH_COUNTS == 'crash-motionx2 ' && $latched3 == 0 ]]; then
    ok "mutant caught: '$EARLY_CRASH_COUNTS' ($EARLY_FAULTS), N+2 camera not latched" \
      "(orient-crash-ensure wants provider/activate and a latched camera)"
  else
    bad "mutant not caught: events '$EARLY_CRASH_COUNTS' ($EARLY_FAULTS); N+2 latched=$latched3"
  fi

  clear_sentinel $PLAIN_BUNDLE
}

# The app makes its first camera call from its CMMotionManager subclass's interval getter, which the stand-in reads
# inside its motion scope. A fault in that call's trap is camera code (SIMCAM_TEST_TRAP_FAULT): it must count against
# the camera, never motion.
c_app-send-crash() {
  orient_early_seq asc SIMCAM_TEST_TRAP_FAULT=1 SIMCAM_TEST_TRAP_FAULT=1 motion-subclass-camera
  local fault='fault in ActEnsure now (main thread, feature key none)'
  early_verdict "$fault; $fault" ''
  clear_sentinel $PLAIN_BUNDLE
}

# Mutant SIMCAM_OFF_APP_SEND_KEY_CLEAR=1 makes the app-answerable sends keep the motion key (it undoes that guard).
# app-send-crash must catch it.
c_mutation-L65() {
  orient_early_seq l65 SIMCAM_TEST_TRAP_FAULT=1 SIMCAM_TEST_TRAP_FAULT=1 motion-subclass-camera \
    SIMCAM_OFF_APP_SEND_KEY_CLEAR=1
  local latched3=$(count_matches 'latched after' $EARLY_DIR3/dylib.log)

  if [[ $EARLY_CRASH_COUNTS == 'crash-motionx2 ' && $EARLY_FAULTS == *'feature key motion'* && $latched3 == 0 ]]; then
    ok "mutant caught: '$EARLY_CRASH_COUNTS' ($EARLY_FAULTS), N+2 camera not latched" \
      "(app-send-crash wants provider/activate, key none, a latched camera)"
  else
    bad "mutant not caught: events '$EARLY_CRASH_COUNTS' ($EARLY_FAULTS); N+2 latched=$latched3"
  fi

  clear_sentinel $PLAIN_BUNDLE
}
