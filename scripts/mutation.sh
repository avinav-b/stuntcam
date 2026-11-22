#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# mutation.sh <rt> [n] — does the canary catch known bugs coming back? (the mutation suite)
#   Most fixes in the engine carry an off switch. Setting one for a launch brings that bug back without a rebuild.
#   For each mutant the canary runs n times (default 1, with SIMCAM_NO_RETRY=1) on a sim armed by
#   scripts/boot.sh <rt> stub. The result is compared with what the mutant's entry expects:
#     catch   A real canary verdict flags it: core=FAIL (core), ext=FAIL (ext), or the named check in the timing list
#             (timing, for a check that can only be a timing one). A PASS is a gap in the canary.
#     miss    A known canary gap, which another fixture guards. A catch means the entry is stale.
#     pass    A positive control: a fault the engine's fix must absorb, so the verdict must be a real PASS. It counts
#             like miss. A catch means the fix stopped working.
#   Some runs count as neither: a run with no real verdict, and an INCONCLUSIVE that doesn't name the timing check.
#   No real verdict is run.sh's "(no verdict" stand-in, for a refused launch, a lost sim or a crash. Such a run is
#   repeated, up to 3 extra runs, and a mutant left with only such runs is UNSURE.
#   A baseline run with no mutant must PASS first, with no `MISMATCH: photo stop` in its console or engine log.
#   run.sh fails such a run too. For example, an exported SIMCTL_CHILD_SIMCAM_TEST_NO_DELEGATE_QUEUE=1 makes the
#   baseline fail.
#   The canary's sentinel state is cleared before every run and at exit. So a crashing run can't latch the next run,
#   or any later run on this sim, into a no-camera FAIL.
#   Exit 1 if any expectation is wrong or UNSURE. Results: one line per mutant.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
RUNTIME=${1:?usage: $0 <rt> [n]}
N=${2:-1}
TAG=mut-$(date +%H%M%S)
EXTRA_RUNS=3   # runs without a real verdict are rerun, at most this many times per mutant

# One mutant per entry, "env|ledger|expect|where[|min runs]", run in this order:
#   env     the settings that bring the bug back (K=V, space-separated)
#   ledger  its regression id (L<nn>, one per fixed bug)
#   expect  catch | miss | pass (see the header)
#   where   which verdict shows it: core | ext | timing:<check text>
#   min     (optional) a mutant whose bug shows only sometimes runs at least this many times, stopping at the first
#           catch once n runs are done
MUTANTS=(
  # exposure bias completion never posted
  "SIMCAM_OFF_BIAS_NOTE=1|L03|catch|ext"

  # isTorchActive not updated on torch off. Two TorchActive posts race on a global queue: NO from AutoTorchEnabled=0,
  # and YES from TorchLevel. The bug shows in about 35% of runs (8 of 23), and the canary flagged every one.
  # 10 runs all miss it with a probability of about 0.65^10 = 1.3%.
  "SIMCAM_OFF_TORCH_LEVEL_ONLY=1|L02|catch|ext|10"

  # focus/exposure adjusting never posted
  "SIMCAM_OFF_SET_PROPERTY_NOTES=1|L01|catch|ext"

  # photo dims ignore maxPhotoDimensions: canary photo-4x3; also `vcfull` photo .dims
  "SIMCAM_OFF_PHOTO_DIMS=1|L06|catch|core"

  # didFinishCaptureFor never fires
  "SIMCAM_OFF_RESPONSIVE_NOTE=1|L07|catch|core"

  # fileDataRepresentation nil
  "SIMCAM_OFF_PHOTO_DATA=1|L05|catch|core"

  # VDO rotation/mirroring not applied: canary front-mirror
  "SIMCAM_OFF_DATA_OUTPUT_ROTATION=1|L30|catch|core"

  # Vision rev pin off: metadata QR finds nothing (canary qr)
  "SIMCAM_OFF_VISION_REVISION_PIN=1|L35|catch|core"

  # movie ignores the active format: canary movie on a non-1080p format; also `vcfull` video .dims
  "SIMCAM_OFF_ACTIVE_FORMAT=1|L16|catch|core"

  # Session teardown not tracked. This is a canary gap: 65 launches that released S2-S4 in turn had no address reuse,
  # so the bug never shows. torture.sh (teardown torture) guards it.
  "SIMCAM_OFF_SESSION_FINALIZE=1|L37|miss|core"

  # preflight fails: must be stock (no camera), never a crash
  "SIMCAM_PF_BREAK=1|L44|catch|core"

  # no preview image with the photo: unguarded (`vcfull` only logs previewImage as INFO)
  "SIMCAM_OFF_PHOTO_PREVIEW=1|L08|miss|core"

  # front preview not mirrored: orient-*.sh guard it
  "SIMCAM_OFF_PREVIEW_MIRROR=1|L30|miss|core"

  # RotationCoordinator stuck at 0°: VC5 orientation guards it
  "SIMCAM_OFF_ROTATION_COORDINATOR=1|L33|miss|core"

  # manual exposure/WB completion never posted: the exposure bias completion check
  "SIMCAM_OFF_MANUAL_COMPLETION=1|L03|catch|ext"

  # photo encode back on the frame pump
  "SIMCAM_OFF_PHOTO_QUEUE=1|L13|catch|timing:longest frame gap during the photo"

  # stop doesn't wait for in-flight photos: canary photo-then-stop
  "SIMCAM_OFF_PHOTO_DRAIN=1|L51|catch|core"

  # stop doesn't wait for the photo's delegate callbacks (DidStopRunning at once), with didFinishCapture handed to its
  # queue 300 ms late so the race always goes the wrong way: canary photo-then-stop "didFinishCaptureFor before
  # didStopRunning" (alone, SIMCAM_OFF_STOP_WAIT lost it on 17.5 in 1 of 11 canary runs)
  "SIMCAM_OFF_STOP_WAIT=1 SIMCAM_TEST_FINISH_DELAY_MS=300|L66|catch|core"

  # Positive control: the same late didFinishCapture, with the fix on. The stop waits for it (about 320 ms), so
  # photo-then-stop passes.
  # Its timing check "stopRunning returned … after didFinishCaptureFor (< 250 ms)" would show a wait that ran to the
  # 500 ms bound, which means a lost done-mark.
  "SIMCAM_TEST_FINISH_DELAY_MS=300|L66|pass|core"

  # no op ring (nothing recorded, no trace file): the canary's trace stage finds no file
  "SIMCAM_TRACE=0|L67|catch|ext"

  # the photo's timestamp stamped after the encode again (~140-480 ms after the slot-12 mark): the trace stage's
  # "timestamp - slot12 (0..50 ms)" and stage order
  "SIMCAM_OFF_GRAB_TIMESTAMP=1|L68|catch|ext"

  # The fix breaks silently: no photo is ever marked done, so every stop with a photo in flight waits the full 500 ms.
  # The order still holds. The canary's stop timing check (< 250 ms) flags it as INCONCLUSIVE.
  "SIMCAM_TEST_LOSE_PHOTO_DONE=1|L66|catch|timing:stopRunning returned"

  # The delegate-queue lookup fails, as if AVFoundation renamed delegateStorage or delegateQueue. The engine logs
  # `MISMATCH: photo stop`, and run.sh turns the run into core=FAIL.
  # The canary itself usually passes, because the race needs a busy main thread.
  "SIMCAM_TEST_NO_DELEGATE_QUEUE=1|L66|catch|core"

  # EXIF DateTimeOriginal at the encode again (100-480 ms after the capture instant): the trace stage's "EXIF
  # DateTimeOriginal - timestamp (-2..2 ms)" (27.0's 10-18 ms encode fit a slot12 + 50 ms window)
  "SIMCAM_OFF_GRAB_EXIF_TIME=1|L72|catch|ext"

  # The trace directory is picked at the first flush again, so a crash in the first 150 ms writes no trace.
  # The canary doesn't crash. activation.sh trace-crash-start (mutation-L69) guards it.
  "SIMCAM_OFF_TRACE_DIR_AT_ARM=1|L69|miss|core"

  # No re-pick when the trace directory goes away mid-run. The canary keeps its state dir.
  # activation.sh trace-repick (mutation-L71) guards it.
  "SIMCAM_OFF_TRACE_REPICK=1|L71|miss|core"

  # Every crashing thread writes the trace from one buffer and one temp file again, with no compare-and-swap.
  # The canary doesn't crash. activation.sh trace-crash2 (mutation-L70) guards it.
  "SIMCAM_OFF_TRACE_CRASH_CLAIM=1|L70|miss|core"

  # A start's flush of a deferred stop is a dispatch_sync on the stop queue again. It returns before a waiter that
  # isn't queued yet. AVFoundation never forwards such a start.
  # activation.sh flush-race (mutation-L73, SIMCAM_TEST_FLUSH_RACE) guards it.
  "SIMCAM_OFF_FLUSH_WAIT=1|L73|miss|core"

  # A start's flush that times out clears the stop's flush mark again. A late waiter then waits for its photos after
  # the start was already reported. AVFoundation never forwards such a start.
  # activation.sh late-waiter (mutation-L74, SIMCAM_TEST_LATE_WAITER_MS) guards it.
  "SIMCAM_OFF_FLUSH_MARK_KEPT=1|L74|miss|core"

  # Activation is unbounded again: an inline dispatch_once on the caller's thread. The canary arms in a few ms either
  # way, so it can't see a missing bound. scripts/activation.sh act-slow-timeout and its mutation-L56 case guard it.
  "SIMCAM_OFF_ACTIVATION_TIMEOUT=1|L56|miss|core"

  # No UI orientation snapshot at load. A coordinator built on a camera queue while main is busy reads portrait
  # preview angles until main is free. The canary builds its coordinator on main.
  # scripts/activation.sh orient-bg and its mutation-L57 case guard it.
  "SIMCAM_OFF_UI_ORIENTATION_AT_LOAD=1|L57|miss|core"

  # CoreMotion ignores the markers at load, so a latched or preflight-failed build keeps the stand-in on every launch.
  # The canary never asks CoreMotion. activation.sh motion-latched and its mutation-L58 case guard it.
  "SIMCAM_OFF_MOTION_MARKERS=1|L58|miss|core"

  # No feature key, so a crash in the stand-in counts as `provider` and latches the camera.
  # The canary never runs the stand-in. activation.sh motion-crash (mutation-L59) guards it.
  "SIMCAM_OFF_MOTION_KEY=1|L59|miss|core"

  # the second motion strike writes no latched-motion marker (stock one launch late): activation.sh motion-crash
  # (mutation-L60) guards it
  "SIMCAM_OFF_HANDLER_MOTION_MARKER=1|L60|miss|core"

  # The orientation tracker's upd runs under the motion key. A camera crash there counts as `motion`, so the camera
  # never latches. The canary never faults there. activation.sh orient-crash (mutation-L61) guards it.
  "SIMCAM_OFF_TRACKER_KEY_CLEAR=1|L61|miss|core"

  # OrientTrackStart keeps the motion key. A tracker fault outside upd, reached from a motion call on main, then counts
  # as `motion`. The canary never faults there. activation.sh orient-crash-main (mutation-L62) guards it.
  "SIMCAM_OFF_TRACKSTART_KEY_CLEAR=1|L62|miss|core"

  # ActEnsure's main-thread tracker run keeps the key: activation.sh orient-crash-ensure (mutation-L63) guards it
  "SIMCAM_OFF_ENSURE_KEY_CLEAR=1|L63|miss|core"

  # markers written in place (mtime = now until futimens), so another process's prune can unlink one being written:
  # activation.sh pf-prune-race (mutation-L64) guards it
  "SIMCAM_OFF_MARKER_RENAME=1|L64|miss|core"

  # the stand-in's sends that app code can answer keep the motion key:
  # activation.sh app-send-crash (mutation-L65) guards it
  "SIMCAM_OFF_APP_SEND_KEY_CLEAR=1|L65|miss|core"

  # interruption end restarts a session the app stopped meanwhile: scripts/bgfg.sh is its guard (fails
  # stopWhileInterrupted and appNotif). The canary can't see the engine keep pumping
  "SIMCAM_OFF_INTERRUPTION_RESET=1|L52|miss|core"

  # maxBracketedCapturePhotoCount reads 8 again, and a bracket hangs. The canary takes no bracket.
  # scripts/features.sh guards it (FAIL bracket).
  "SIMCAM_OFF_BRACKET_UNSUPPORTED=1|L53|miss|core"

  # 29 multi-cam device sets again, and startRunning fails with -11873. The canary runs no MultiCam session.
  # scripts/features.sh guards it (FAIL multicam).
  "SIMCAM_OFF_MULTICAM_UNSUPPORTED=1|L54|miss|core"
)

# Not need_udid: here the UDID only locates the canary's sentinel state. When it is unset, nothing is cleared, and
# run.sh reports the missing UDID.
UDID_VAR=SIMCAM_UDID_$RUNTIME
UDID=${(P)UDID_VAR:-}   # (P): the value of the variable UDID_VAR names
CANARY_STATE=$SIMCAM_HOME/sims/$UDID/state/dev.simcam.spike.m10canary
clear_sentinel() { [[ -n $UDID ]] && rm -rf "$CANARY_STATE"; }
trap clear_sentinel EXIT

# verdict <run tag>: that canary run's summary line (empty if the run left none)
verdict() { cat "$SIMCAM_RESULTS/canary-$RUNTIME-boot-$TAG-$1/summary.txt" 2>/dev/null; }

# classify <verdict> <where>: one run's verdict for a mutant shown by <where>:
# caught | clean (a real PASS of that verdict) | unsure
classify() {
  local summary=$1 where=$2
  if [[ $summary != RESULT* || $summary == *"(no verdict"* ]]; then
    echo unsure
    return
  fi
  case $where in
    core)
      if [[ $summary == *core=FAIL* ]]; then
        echo caught
      elif [[ $summary == *core=PASS* ]]; then
        echo clean
      else
        echo unsure
      fi
      ;;
    ext)
      if [[ $summary == *ext=FAIL* ]]; then
        echo caught
      elif [[ $summary == *core=PASS*ext=PASS* ]]; then
        echo clean
      else
        echo unsure
      fi
      ;;
    timing:*)
      if [[ $summary == *timing:\[*${where#timing:}* ]]; then
        echo caught
      elif [[ $summary == *core=PASS* ]]; then
        echo clean
      else
        echo unsure
      fi
      ;;
  esac
}

# ---------------------------------------------------------------- the baseline: no mutant must PASS
clear_sentinel
# its output stays in its result dir; the verdict is read from there
XENV= SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh canary $RUNTIME boot $TAG-base >/dev/null 2>&1
base_verdict=$(verdict base)
BASE_DIR=$SIMCAM_RESULTS/canary-$RUNTIME-boot-$TAG-base

# a log the run did not write has no MISMATCH line
base_mismatch=$(grep -h -m1 -oE 'MISMATCH: photo stop[^;]{0,120}' $BASE_DIR/app.log $BASE_DIR/dylib.log 2>/dev/null |
  head -1)
if [[ -n $base_mismatch ]]; then
  echo "baseline logged an engine MISMATCH (a fix is off): $base_mismatch; verdict ${base_verdict:0:200}"
  exit 2
fi
if [[ $base_verdict != *core=PASS*ext=PASS* ]]; then
  echo "baseline did not pass: ${base_verdict:0:200}"
  exit 2
fi
echo "baseline PASS"

# ---------------------------------------------------------------- the mutants
wrong=0
for mutant in $MUTANTS; do
  IFS='|' read setting ledger want where min <<<"$mutant"
  name=${setting%%=*}
  caught=0
  clean=0
  unsure=0
  last_summary=

  runs=$N
  (( ${min:-0} > N )) && runs=$min

  for i in {1..$(( runs + EXTRA_RUNS ))}; do
    (( caught > 0 && caught + clean >= N )) && break        # caught, and at least n real verdicts
    (( caught + clean >= runs )) && break                   # enough real verdicts
    # every run: a crashing run must not latch the next one into a no-camera FAIL that counts as a catch
    clear_sentinel
    # its output stays in its result dir; the verdict is read from there
    XENV="$setting" SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh canary $RUNTIME boot $TAG-$name-$i >/dev/null 2>&1

    summary=$(verdict $name-$i)
    last_summary=$summary
    class=$(classify "$summary" "$where")
    # the positive control's wait hit the bound
    [[ $want == pass && $summary == *timing:\[*"stopRunning returned"* ]] && class=caught

    case $class in
      caught)
        caught=$((caught + 1))
        ;;
      clean)
        clean=$((clean + 1))
        ;;
      *)
        unsure=$((unsure + 1))
        ;;
    esac
  done

  if (( caught )); then
    got=catch
  elif (( clean )); then
    got=miss
  else
    got=unsure
  fi
  [[ $want == pass && $got == miss ]] && got=pass   # a positive control's clean runs are its expected PASS

  if [[ $got == $want ]]; then
    outcome=ok
  elif [[ $got == unsure ]]; then
    outcome=UNSURE
    wrong=$((wrong + 1))
  else
    outcome=WRONG
    wrong=$((wrong + 1))
  fi

  printf '%-6s %-33s %-4s expect=%-5s got=%-6s (caught %d, clean %d, unsure %d) %s\n' \
    $outcome $name $ledger $want $got $caught $clean $unsure "$(print -r -- $last_summary | cut -c1-140)"
done

if (( wrong )); then
  echo "$wrong mutant(s) did not behave as the ledger expects (or had no real verdict)"
  exit 1
fi
echo "all ${#MUTANTS} mutants as expected"
