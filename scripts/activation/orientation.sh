# SPDX-License-Identifier: Apache-2.0
# activation/orientation.sh — the UI orientation snapshot and faults in the orientation tracker and other camera code.
# Sourced by scripts/activation.sh, which runs each case <c> as the function c_<c>.
c_orient-nomotion() {
  clear_sentinel $BO; p12 orient landscapeLeft nm1 SIMCAM_OFF_MOTION=1; local a1=$(angles $PD) d1=$PD
  p12 orient landscapeLeft nm0; local a0=$(angles $PD)
  if [[ -n $a1 && $a1 == $a0 && $a1 != *captureAngle=90* && $(cnt 'motion: hooked' $d1/app.log) == 0 && $(cnt 'P12\| DONE' $d1/app.log) == 1 ]]
  then ok "SIMCAM_OFF_MOTION=1 (CoreMotion stock): '$a1', same as without the switch"
  else bad "no-motion '$a1' vs with motion '$a0' (hooked=$(cnt 'motion: hooked' $d1/app.log))"; fi
  clear_sentinel $BO
}
c_orient-sameturn() {
  clear_sentinel $BO; p12 coord-first landscapeLeft sf P12_UI=landscape
  local l=$(grep -m1 -E '(PASS|FAIL) coord-first' $PD/app.log)
  [[ $l == *PASS* ]] && ok "${l#*\| } ($(grep -m1 'COORD same-turn' $PD/app.log | grep -oE 'ui=[0-9]+ dev=[0-9]+'))" ||
    bad "${l:-no verdict}; $(grep 'COORD' $PD/app.log | tr '\n' ' ' | cut -c1-200)"
  clear_sentinel $BO
}
c_orient-bg() {
  clear_sentinel $BO; p12 coord-bg landscapeLeft cb P12_UI=landscape
  local l=$(grep -m1 -E '(PASS|FAIL) coord-bg' $PD/app.log)
  local detail="$(grep 'COORD' $PD/app.log | sed 's/^P12| //' | tr '\n' ';' | cut -c1-240) $(grep -m1 -oE 'ui orientation at load: .*' $PD/dylib.log)"
  [[ $l == *PASS* ]] && ok "${l#*\| } — $detail" || bad "${l:-no verdict} — $detail"
  clear_sentinel $BO
}
c_orient-bg-turnback() {   # the UI turns landscape, then back to portrait, before the first camera call: the snapshot follows both
  clear_sentinel $BO; p12 coord-bg landscapeLeft tb P12_UI=turnback
  local l=$(grep -m1 -E '(PASS|FAIL) coord-bg-turnback' $PD/app.log) snap=$(grep -m1 -oE 'ui orientation at load: .*' $PD/dylib.log)
  local detail="$(grep -E 'COORD|turnback ui' $PD/app.log | sed 's/^P12| //' | tr '\n' ';' | cut -c1-240) $snap"
  [[ $l == *PASS* && $snap == *'then 3 (geometry change), then 1 (geometry change)'* ]] && ok "${l#*\| } — $detail" ||
    bad "${l:-no verdict} — $detail"
  clear_sentinel $BO
}
c_mutation-L57() {   # SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1: no UI orientation snapshot at load: orient-bg must catch it
  clear_sentinel $BO; p12 coord-bg landscapeLeft l57 P12_UI=landscape SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1
  local l=$(grep -m1 -E '(PASS|FAIL) coord-bg' $PD/app.log)
  if [[ $l == *FAIL*preview=90* && $(cnt 'ui orientation at load: off \(SIMCAM_OFF_UI_ORIENTATION_AT_LOAD\)' $PD/dylib.log) == 1 ]]
  then ok "mutant caught: ${l#*\| } (the coordinator on the camera queue reads portrait until main frees)"
  else bad "mutant not caught: ${l:-no verdict}; $(grep -m1 -oE 'ui orientation at load: .*' $PD/dylib.log)"; fi
  clear_sentinel $BO
}
orient_crash_seq() {   # orient_crash_seq <tag> [K=V ...]: plain-orient coord-bg (armed, a coordinator) faulting in the tracker's upd
  # at N and N+1, then N+2 without the fault. Sets OC_EV (crash lines of N, N+1) OC_PIDS (the fault lines' pids) OC_D3
  local t=$1; shift; local e=$(ev $BO) n0; clear_sentinel $BO; n0=$(nl $e)
  p12 coord-bg landscapeLeft $t-1 P12_UI=landscape SIMCAM_TEST_ORIENT_FAULT=1 "$@"; OC_PIDS=$(fault_pid $PD/app.log 'SIMCAM_TEST_ORIENT_FAULT: fault')
  p12 coord-bg landscapeLeft $t-2 P12_UI=landscape SIMCAM_TEST_ORIENT_FAULT=1 "$@"; OC_PIDS="$OC_PIDS $(fault_pid $PD/app.log 'SIMCAM_TEST_ORIENT_FAULT: fault')"
  OC_EV=$(since $e $n0 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' ')
  p12 coord-bg landscapeLeft $t-3 P12_UI=landscape "$@"; OC_D3=$PD
}
c_orient-crash() {   # a fault in the orientation tracker (camera code) counts against the camera, never `motion`: N+2 is camera-latched
  orient_crash_seq oc; local l3=$(grep -m1 -E '(PASS|FAIL) coord-bg' $OC_D3/app.log) eng=0; local -a pp; pp=(${=OC_PIDS}); crash_reports p12 30 $pp
  [[ $OC_EV == (crash-providerx2|crash-activatex2|crash-activatex1 crash-providerx1)' ' && ${#pp} == 2 &&
    $(cnt 'latched after' $OC_D3/dylib.log) == 1 && $l3 == *'FAIL coord-bg no device'* ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OC_PIDS) faulted in upd after the coordinators' KVO: $OC_EV(no motion), $CR crash reports; N+2 camera-latched in PREPARE 1 (${l3#*\| })" \
    "events '$OC_EV' fault pids '$OC_PIDS' reports=$CR; N+2 latched=$(cnt 'latched after' $OC_D3/dylib.log) '${l3:-no verdict}'"
  clear_sentinel $BO
}
c_mutation-L61() {   # SIMCAM_OFF_TRACKER_KEY_CLEAR=1: upd under the motion key again: orient-crash must catch it
  orient_crash_seq l61 SIMCAM_OFF_TRACKER_KEY_CLEAR=1; local l3=$(grep -m1 -E '(PASS|FAIL) coord-bg' $OC_D3/app.log)
  if [[ $OC_EV == 'crash-motionx2 ' && $(cnt 'latched after' $OC_D3/dylib.log) == 0 ]]
  then ok "mutant caught: '$OC_EV', N+2 camera not latched (${l3#*\| }; orient-crash wants provider and a latched camera)"
  else bad "mutant not caught: events '$OC_EV'; N+2 latched=$(cnt 'latched after' $OC_D3/dylib.log) '${l3:-no verdict}'"; fi
  clear_sentinel $BO
}
crash_ev() { since $1 $2 | awk '$3 ~ /^crash/ {print $3 "-" $4}' | sort | uniq -c | awk '{print $2 "x" $1}' | tr '\n' ' '; }
camera_ev_ok() { [[ $1 == (crash-providerx2|crash-activatex2|crash-activatex1 crash-providerx1)' ' ]]; }
orient_early_seq() {   # orient_early_seq <tag> <fault env N> <fault env N+1> <order> [K=V ...]: N, N+1 fault in camera code outside
  # upd (SIMCAM_TEST_ORIENT_FAULT=<variant>, SIMCAM_TEST_TRAP_FAULT=1), N+2 plain camera-then-motion. Sets OE_EV OE_F (fault lines of N, N+1)
  # OE_PIDS (their pids) OE_D3. The crash reports (by those pids) are counted only by the cases that want them (CR, CR_NO).
  local t=$1 v1=$2 v2=$3 o=$4; shift 4; local e=$(ev $BP) n0 rx='SIMCAM_TEST_[A-Z_]+: fault in [A-Za-z0-9]+ now \([^)]*\)'; clear_sentinel $BP; n0=$(nl $e)
  run plain $t-1 M5APP_ORDER=$o $v1 "$@"; local d1=$(res plain $t-1)
  run plain $t-2 M5APP_ORDER=$o $v2 "$@"; local d2=$(res plain $t-2)
  OE_EV=$(crash_ev $e $n0)
  OE_F="$(grep -m1 -oE $rx $d1/app.log | sed 's/^SIMCAM_TEST_[A-Z_]*: //'); $(grep -m1 -oE $rx $d2/app.log | sed 's/^SIMCAM_TEST_[A-Z_]*: //')"
  OE_PIDS="$(fault_pid $d1/app.log 'SIMCAM_TEST_[A-Z_]+: fault in') $(fault_pid $d2/app.log 'SIMCAM_TEST_[A-Z_]+: fault in')"
  run plain $t-3 M5APP_ORDER=camera-then-motion M5APP_EXPECT=stock "$@"; OE_D3=$(res plain $t-3)
}
# oe_reports: crash_reports for OE_PIDS (sets CR, CR_NO), and OE_NP: how many pids there are
oe_reports() { local -a pp; pp=(${=OE_PIDS}); crash_reports m5app 30 $pp; OE_NP=${#pp}; }
c_orient-crash-main() {   # the first motion call on main: faults in OrientTrackStart (N) and OrientTrackRunMain before upd (N+1) are camera
  orient_early_seq ocm SIMCAM_TEST_ORIENT_FAULT=start SIMCAM_TEST_ORIENT_FAULT=runmain motion-main-camera-bg
  local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $OE_D3/app.log) eng=0; oe_reports
  camera_ev_ok "$OE_EV" && [[ $OE_NP == 2 &&
    $OE_F == 'fault in OrientTrackStart now (main thread); fault in OrientTrackRunMain now (main thread)' &&
    $(cnt 'latched after' $OE_D3/dylib.log) == 1 && $disc == *=0 ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OE_PIDS): $OE_F; events $OE_EV(no motion), $CR crash reports; N+2 camera-latched in PREPARE 1 ($disc)" \
    "faults '$OE_F' (pids '$OE_PIDS') events '$OE_EV' reports=$CR; N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log) $disc"
  clear_sentinel $BP
}
c_mutation-L62() {   # SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1: OrientTrackStart keeps the motion key (motion.inc's clear deleted): orient-crash-main must catch it
  orient_early_seq l62 SIMCAM_TEST_ORIENT_FAULT=start SIMCAM_TEST_ORIENT_FAULT=runmain motion-main-camera-bg SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1
  if [[ $OE_EV == 'crash-motionx2 ' && $(cnt 'latched after' $OE_D3/dylib.log) == 0 ]]
  then ok "mutant caught: '$OE_EV' ($OE_F), N+2 camera not latched (orient-crash-main wants provider/activate and a latched camera)"
  else bad "mutant not caught: events '$OE_EV' ($OE_F); N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log)"; fi
  clear_sentinel $BP
}
c_orient-crash-ensure() {   # the first camera call on main arriving with the motion key set: ActEnsure's tracker run is camera
  orient_early_seq oce SIMCAM_TEST_ORIENT_FAULT=ensure SIMCAM_TEST_ORIENT_FAULT=ensure camera-then-motion
  local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $OE_D3/app.log) eng=0; oe_reports
  camera_ev_ok "$OE_EV" && [[ $OE_NP == 2 &&
    $OE_F == 'fault in OrientTrackRunMain now (main thread); fault in OrientTrackRunMain now (main thread)' &&
    $(cnt 'latched after' $OE_D3/dylib.log) == 1 && $disc == *=0 ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OE_PIDS): $OE_F; events $OE_EV(no motion), $CR crash reports; N+2 camera-latched ($disc)" \
    "faults '$OE_F' (pids '$OE_PIDS') events '$OE_EV' reports=$CR; N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log) $disc"
  clear_sentinel $BP
}
c_mutation-L63() {   # SIMCAM_OFF_ENSURE_KEY_CLEAR=1: ActEnsure's main-thread tracker run keeps the key (lazy.inc's clear deleted): orient-crash-ensure must catch it
  orient_early_seq l63 SIMCAM_TEST_ORIENT_FAULT=ensure SIMCAM_TEST_ORIENT_FAULT=ensure camera-then-motion SIMCAM_OFF_ENSURE_KEY_CLEAR=1
  if [[ $OE_EV == 'crash-motionx2 ' && $(cnt 'latched after' $OE_D3/dylib.log) == 0 ]]
  then ok "mutant caught: '$OE_EV' ($OE_F), N+2 camera not latched (orient-crash-ensure wants provider/activate and a latched camera)"
  else bad "mutant not caught: events '$OE_EV' ($OE_F); N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log)"; fi
  clear_sentinel $BP
}
c_app-send-crash() {   # the app's first camera call from its CMMotionManager subclass's interval getter, which
  # the stand-in reads inside its motion scope; a fault in that call's trap (camera code, SIMCAM_TEST_TRAP_FAULT) must be camera, never motion
  orient_early_seq asc SIMCAM_TEST_TRAP_FAULT=1 SIMCAM_TEST_TRAP_FAULT=1 motion-subclass-camera
  local disc=$(grep -oE 'camera-then-motion discovery=[0-9]+' $OE_D3/app.log) eng=0; oe_reports
  camera_ev_ok "$OE_EV" && [[ $OE_NP == 2 &&
    $OE_F == 'fault in ActEnsure now (main thread, feature key none); fault in ActEnsure now (main thread, feature key none)' &&
    $(cnt 'latched after' $OE_D3/dylib.log) == 1 && $disc == *=0 ]] && eng=1
  verdict_reports $eng 2 "N, N+1 (pids $OE_PIDS): $OE_F; events $OE_EV(no motion), $CR crash reports; N+2 camera-latched ($disc)" \
    "faults '$OE_F' (pids '$OE_PIDS') events '$OE_EV' reports=$CR; N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log) $disc"
  clear_sentinel $BP
}
c_mutation-L65() {   # SIMCAM_OFF_APP_SEND_KEY_CLEAR=1: the app-answerable sends keep the motion key (that guard undone): app-send-crash must catch it
  orient_early_seq l65 SIMCAM_TEST_TRAP_FAULT=1 SIMCAM_TEST_TRAP_FAULT=1 motion-subclass-camera SIMCAM_OFF_APP_SEND_KEY_CLEAR=1
  if [[ $OE_EV == 'crash-motionx2 ' && $OE_F == *'feature key motion'* &&
    $(cnt 'latched after' $OE_D3/dylib.log) == 0 ]]
  then ok "mutant caught: '$OE_EV' ($OE_F), N+2 camera not latched (app-send-crash wants provider/activate, key none, a latched camera)"
  else bad "mutant not caught: events '$OE_EV' ($OE_F); N+2 latched=$(cnt 'latched after' $OE_D3/dylib.log)"; fi
  clear_sentinel $BP
}
