#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# mutation.sh <rt> [n] — does the canary catch known bugs coming back? (design §5.3 mutation suite; docs/ledger.md)
#   Most fixes in the engine still carry their spike's off switch; setting one per launch brings that bug back without a
#   rebuild. For each mutant the canary runs n times (default 1, SIMCAM_NO_RETRY=1) on a sim armed by scripts/boot.sh <rt> stub,
#   and the result is compared with what the ledger expects:
#     catch   a real canary verdict flags it: core=FAIL (core), ext=FAIL (ext), or the named timing check in the timing list
#             (timing: the check can only be a timing one)                                 -> a PASS is a ledger gap
#     miss    a known canary gap (another fixture guards it)                              -> a catch means the ledger is stale
#     pass    a positive control: a fault the engine's fix must absorb, so the verdict must be a real PASS (counted
#             like miss; a catch means the fix stopped working)
#   A run with no real verdict (run.sh's "(no verdict" stand-in: refused launch, lost sim, crash) or an INCONCLUSIVE that
#   doesn't name the timing check counts as neither: it is rerun, up to 3 extra runs, and a mutant left with only such runs
#   is UNSURE. A baseline run (no mutant) must PASS first, with no `MISMATCH: w4 e40` in its console or engine log (w4 e54:
#   run.sh fails such a run too; e.g. SIMCTL_CHILD_M40_NO_DELEGATEQ=1 exported makes the baseline fail). The canary's sentinel state is cleared before every run and at
#   exit, so a crashing run can't latch the next one (or later runs on this sim) into a no-camera FAIL.
#   Exit 1 if any expectation is wrong or UNSURE. Results: one line per mutant.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh
RT=${1:?usage: $0 <rt> [n]}; N=${2:-1}; TAG=mut-$(date +%H%M%S)
# env | ledger | expect | which verdict shows it (core|ext|timing:<check text>) [| min runs: a mutant whose bug shows only
#   sometimes runs at least this many times, stopping at the first catch once n runs are done]
MUTANTS=(
  "M8_NO_BIASNOTE=1|L03|catch|ext"         # exposure bias completion never posted
  "M8_OLD_TORCH=1|L02|catch|ext|10"        # isTorchActive not updated on torch off. Two TorchActive posts (NO from
                                           # AutoTorchEnabled=0, YES from TorchLevel) race on a global queue: the bug shows in ~35%
                                           # (8/23, w2 e11), the canary flagged every one; 10 runs miss it with p ~ 0.65^10 = 1.3%
  "M5_NO_METERING=1|L01|catch|ext"         # focus/exposure adjusting never posted
  "M8_NO_PHOTODIMS=1|L06|catch|core"       # photo dims ignore maxPhotoDimensions: canary photo-4x3 (w2; a gap in e11, where the
                                           # canary's 16:9 photo matched the 4032x2268 fallback); also `vcfull` photo .dims
  "M10_NO_RESPONSIVE=1|L07|catch|core"     # didFinishCaptureFor never fires
  "M5_NO_PHOTOFIX=1|L05|catch|core"        # fileDataRepresentation nil
  "M7_NO_VDOROT=1|L30|catch|core"          # VDO rotation/mirroring not applied: canary front-mirror
  "M7_NO_VISIONFIX=1|L35|catch|core"       # Vision rev pin off: metadata QR finds nothing (canary qr)
  "M8_NO_FORMATDIMS=1|L16|catch|core"      # movie ignores the active format: canary movie on a non-1080p format (w2; a gap in e11,
                                           # where the canary's 1080p movie matched the fallback); also `vcfull` video .dims
  "M10_NO_FINALIZE=1|L37|miss|core"        # session teardown not tracked: a canary gap: 0 address reuses in 65 launches with S2-S4
                                           # released in turn (w3 e12), so the bug never shows; torture.sh (teardown torture) guards it
  "SIMCAM_PF_BREAK=1|L44|catch|core"       # preflight fails: must be stock (no camera), never a crash
  "M8_NO_PREVIEWSURFACE=1|L08|miss|core"   # no preview image with the photo: unguarded (`vcfull` only logs previewImage as INFO)
  "M12_NO_PREVIEWMIRROR=1|L30|miss|core"   # front preview not mirrored: orient-*.sh guard it
  "M5_NO_COORD=1|L33|miss|core"            # RotationCoordinator stuck at 0°: VC5 orientation guards it
  "M8_NO_MANUALCOMPLETE=1|L03|catch|ext"   # manual exposure/WB completion never posted: the L03 bias completion check (w2 e10a, e11)
  "M13_PHOTO_ON_PUMP=1|L13|catch|timing:longest frame gap during the photo"   # photo encode back on the frame pump
  "M13_NO_PHOTODRAIN=1|L51|catch|core"     # stop doesn't wait for in-flight photos: canary photo-then-stop
  "M40_OLD_STOP=1 M40_FINISH_DELAY_MS=300|L66|catch|core"   # w4 e40: stop doesn't wait for the photo's delegate callbacks
                                           # (DidStopRunning at once), with didFinishCapture handed to its queue 300 ms late
                                           # so the race always goes the wrong way: canary photo-then-stop "didFinishCaptureFor
                                           # before didStopRunning" (alone, M40_OLD_STOP lost it on 17.5 in 1 of 11 canary runs)
  "M40_FINISH_DELAY_MS=300|L66|pass|core"  # w4 e44 positive control (review M2): the same late didFinishCapture with the fix on:
                                           # the stop waits for it (~320 ms), so photo-then-stop passes, and its
                                           # "stopRunning returned … after didFinishCaptureFor (< 250 ms)" timing check shows a
                                           # wait that ran to the 500 ms bound (a lost done-mark; 1 s before w4 e54)
  "SIMCAM_TRACE=0|L67|catch|ext"           # w4 e48: no op ring (nothing recorded, no trace file): the canary's trace stage finds no file
  "M48_OLD_PTS=1|L68|catch|ext"            # w4 e48: the photo's timestamp stamped after the encode again (~140-480 ms after the
                                           # slot-12 mark): the trace stage's "timestamp - slot12 (0..50 ms)" and stage order
  "M40_LOSE_DONE=1|L66|catch|timing:stopRunning returned"   # w4 e44: the fix breaks silently: no photo is ever marked done, so
                                           # every stop with a photo in flight waits the full 500 ms bound; the order still
                                           # holds, the canary's stop timing check (< 250 ms) flags it (INCONCLUSIVE)
  "M40_NO_DELEGATEQ=1|L66|catch|core"      # w4 e54: the delegate-queue lookup fails (as if AVF renamed delegateStorage/delegateQueue):
                                           # the engine logs `MISMATCH: w4 e40` and run.sh turns the run into core=FAIL (the
                                           # canary itself usually passes: the e40 race needs a busy main)
  "M54_OLD_EXIF=1|L72|catch|ext"           # w4 e54: EXIF DateTimeOriginal at the encode again (100-480 ms after the capture instant):
                                           # the trace stage's "EXIF DateTimeOriginal - timestamp (-2..2 ms)" (27.0's 10-18 ms
                                           # encode fit a slot12 + 50 ms window, e54)
  "M54_LATE_TRACE_DIR=1|L69|miss|core"     # w4 e54: the trace directory picked at the first flush again (a crash in the first 150 ms
                                           # writes no trace): the canary doesn't crash; activation.sh trace-crash-start
                                           # (mutation-L69) guards it
  "M54_NO_REPICK=1|L71|miss|core"          # w4 e54: no re-pick when the trace directory goes away mid-run: the canary keeps its
                                           # state dir; activation.sh trace-repick (mutation-L71) guards it
  "M54_OLD_CRASH=1|L70|miss|core"          # w4 e57: every crashing thread writes the trace from one buffer and temp file again (no
                                           # CAS): the canary doesn't crash; activation.sh trace-crash2 (mutation-L70) guards it
  "M54_OLD_FLUSH=1|L73|miss|core"          # w4 e54: a start's flush of a deferred stop is a dispatch_sync on the stop queue again
                                           # (returns before a waiter not yet queued): AVF never forwards such a start;
                                           # activation.sh flush-race (mutation-L73, M54_FLUSH_RACE) guards it
  "M59_OLD_FLUSHMARK=1|L74|miss|core"      # w4 e59: a start's flush that times out clears the stop's flush mark again, so a late
                                           # waiter waits for its photos with the start already reported: AVF never forwards
                                           # such a start; activation.sh late-waiter (mutation-L74, M59_LATE_WAITER) guards it
  "M16_NO_ACTTIMEOUT=1|L56|miss|core"      # activation unbounded again (today's inline dispatch_once on the caller's thread): the
                                           # canary arms in a few ms either way, so it can't see a missing bound;
                                           # scripts/activation.sh act-slow-timeout (and its mutation-L56 case) is the guard (W4)
  "M16_NO_UIORIENT_AT_LOAD=1|L57|miss|core" # no UI orientation snapshot at load: a coordinator built on a camera queue while main
                                           # is busy reads portrait preview angles until main frees. The canary builds its
                                           # coordinator on main; scripts/activation.sh orient-bg (and its mutation-L57 case) guards it
  "M17_NO_MOTION_MARKERS=1|L58|miss|core"  # W4 PR B: CoreMotion ignores markers at load (a latched or preflight-failed build keeps the
                                           # stand-in on every launch): the canary never asks CoreMotion; activation.sh
                                           # motion-latched (and its mutation-L58 case) guards it
  "M17_NO_MOTION_KEY=1|L59|miss|core"      # W4 PR B: no feature key: a stand-in crash is `provider` and latches the camera: the canary
                                           # never runs the stand-in; activation.sh motion-crash (mutation-L59) guards it
  "M17_NO_HANDLER_MARKER=1|L60|miss|core"  # W4 PR B: the second motion strike writes no latched-motion marker (stock one launch late):
                                           # activation.sh motion-crash (mutation-L60) guards it
  "M17_TRACKER_MOTION_SCOPE=1|L61|miss|core" # review: the orientation tracker's upd runs under the motion key, so a camera crash
                                           # there is `motion` and the camera never latches: the canary never faults there;
                                           # activation.sh orient-crash (mutation-L61) guards it
  "M17_TRACKSTART_KEEP_KEY=1|L62|miss|core" # e29: M5OrientTrackStart keeps the motion key (a tracker fault outside upd, reached from a
                                           # motion call on main, is `motion`): the canary never faults there; activation.sh
                                           # orient-crash-main (mutation-L62) guards it
  "M17_ENSURE_KEEP_KEY=1|L63|miss|core"    # e29: ActEnsure's main-thread tracker run keeps the key: activation.sh orient-crash-ensure
                                           # (mutation-L63) guards it
  "M17_MARKER_INPLACE=1|L64|miss|core"     # e29: markers written in place (mtime = now until futimens), so another process's prune
                                           # can unlink one being written: activation.sh pf-prune-race (mutation-L64) guards it
  "M17_APP_SEND_KEEP_KEY=1|L65|miss|core"  # e31: the stand-in's sends that app code can answer keep the motion key (low 3 undone):
                                           # activation.sh app-send-crash (mutation-L65) guards it
  "M14_OLD_ENDINT=1|L52|miss|core"         # interruption end restarts a session the app stopped meanwhile: scripts/bgfg.sh is its guard
                                           # (fails stopWhileInterrupted and appNotif; w3 e21). The canary can't see the engine keep
                                           # pumping (w3 e20)
  "M15_OLD_BRACKET=1|L53|miss|core"        # w4 e22: maxBracketedCapturePhotoCount reads 8 again (a bracket hangs): the canary takes
                                           # no bracket; scripts/features.sh is its guard (FAIL bracket, w4 e23b)
  "M15_OLD_MULTICAM=1|L54|miss|core"       # w4 e22: 29 multi-cam device sets again (startRunning -11873): the canary runs no
                                           # MultiCam session; scripts/features.sh is its guard (FAIL multicam, w4 e23b)
)
UDID=${(P)${:-SIMCAM_UDID_$RT}:-}; CST=$SIMCAM_HOME/sims/$UDID/state/dev.simcam.spike.m10canary
clear_sentinel() { [[ -n $UDID ]] && rm -rf "$CST"; }
trap clear_sentinel EXIT
verdict() { cat "$SIMCAM_RESULTS/canary-$RT-boot-$TAG-$1/summary.txt" 2>/dev/null; }
# classify one run's verdict for a mutant shown by $where: caught | clean (a real PASS of that verdict) | unsure
classify() {
  local v=$1 where=$2
  [[ $v == RESULT* && $v != *"(no verdict"* ]] || { echo unsure; return; }
  case $where in
    core) [[ $v == *core=FAIL* ]] && echo caught || { [[ $v == *core=PASS* ]] && echo clean || echo unsure; };;
    ext)  [[ $v == *ext=FAIL* ]] && echo caught || { [[ $v == *core=PASS*ext=PASS* ]] && echo clean || echo unsure; };;
    timing:*) [[ $v == *timing:\[*${where#timing:}* ]] && echo caught || { [[ $v == *core=PASS* ]] && echo clean || echo unsure; };;
  esac
}
clear_sentinel
XENV= SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh canary $RT boot $TAG-base >/dev/null 2>&1
b=$(verdict base); BD=$SIMCAM_RESULTS/canary-$RT-boot-$TAG-base
bm=$(grep -h -m1 -oE 'MISMATCH: w4 e40[^;]{0,120}' $BD/app.log $BD/dylib.log 2>/dev/null | head -1)
[[ -z $bm ]] || { echo "baseline logged an engine MISMATCH (a fix is off): $bm; verdict ${b:0:200}"; exit 2; }
[[ $b == *core=PASS*ext=PASS* ]] || { echo "baseline did not pass: ${b:0:200}"; exit 2; }
echo "baseline PASS"; wrong=0
for m in $MUTANTS; do
  IFS='|' read env led want where min <<<"$m"; name=${env%%=*}; caught=0; clean=0; unsure=0; lastv=; runs=$N
  (( ${min:-0} > N )) && runs=$min
  for i in {1..$(( runs + 3 ))}; do
    (( caught > 0 && caught + clean >= N )) && break        # caught, and at least n real verdicts
    (( caught + clean >= runs )) && break                   # enough real verdicts
    clear_sentinel   # every run: a crashing run must not latch the next one into a no-camera FAIL that counts as a catch
    XENV="$env" SIMCAM_NO_RETRY=1 $ROOT/scripts/run.sh canary $RT boot $TAG-$name-$i >/dev/null 2>&1
    v=$(verdict $name-$i); lastv=$v
    c=$(classify "$v" "$where")
    [[ $want == pass && $v == *timing:\[*"stopRunning returned"* ]] && c=caught   # the positive control's wait hit the bound
    case $c in caught) caught=$((caught + 1));; clean) clean=$((clean + 1));; *) unsure=$((unsure + 1));; esac
  done
  if (( caught )); then got=catch; elif (( clean )); then got=miss; else got=unsure; fi
  [[ $want == pass && $got == miss ]] && got=pass   # a positive control's clean runs are its expected PASS
  if [[ $got == $want ]]; then st=ok; elif [[ $got == unsure ]]; then st=UNSURE; wrong=$((wrong + 1)); else st=WRONG; wrong=$((wrong + 1)); fi
  printf '%-6s %-24s %-4s expect=%-5s got=%-6s (caught %d, clean %d, unsure %d) %s\n' $st $name $led $want $got $caught $clean $unsure "$(print -r -- $lastv | cut -c1-140)"
done
(( wrong )) && { echo "$wrong mutant(s) did not behave as the ledger expects (or had no real verdict)"; exit 1; } || echo "all ${#MUTANTS} mutants as expected"
