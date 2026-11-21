#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# golden.sh capture <label> [loader] | compare <labelA> <labelB> — golden frames for the scene, image and video sources:
# the hashes of every frame the engine delivers to an app's video data output, per case (source x format x
# orientation x rotation/mirroring), so that a change to another source's path can show these are untouched.
#   capture  runs fixtures/plain in M5APP_MODE=golden (golden.swift) for each case below on the sims $SIMCAM_UDID_<rt>
#            (RTS, default "263 27"; armed with boot.sh <rt> stub). With [loader] the sims use that build for the
#            capture (swap.sh) and go back to their previous one afterwards. Writes
#            $SIMCAM_RESULTS/golden-<label>/<rt>-<case>.txt: one "<hash>-<WxH>" per distinct frame, "photo <hash>" for the
#            cases that take a photo, and "movie <index> <hash>-<WxH>" for the first 48 frames of a 2 s movie, decoded
#            (both for the image source only, whose picture is static; the movie covers audio.inc's append path and its
#            conversion; by index, because the encoder refines a still picture frame by frame).
#   compare  per case, whether the two captures hold the same set of distinct frames (and photo and movie frames),
#            and that each has every case of the other. Exit 1 on any difference. Frame counts are not compared:
#            which frames of a moving source fall inside the window depends on timing, so the scene and video runs
#            are long enough to see every frame they have (the scene's moving bar repeats every 160 frames; the video
#            is $SIMCAM_HOME/golden/slow10.mov, 30 frames at 10 fps, made once with tools/mkvideo, so each of its
#            frames lasts 3 ticks).
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh
CMD=${1:?capture|compare}; shift
B=dev.simcam.spike.m10plain
# case: name W H fps fmt pos rot mirror orientation (the source comes from SOURCES)
CASES=(
  "p1080bgra   1920 1080 30 bgra back  90  0 portrait"
  "p1080v      1920 1080 30 420v back  0   0 portrait"
  "l4kv        3840 2160 30 420v back  0   0 landscapeRight"
  "f1080mir    1920 1080 30 bgra front 90  1 portrait"
  "u1440f      1920 1440 30 420f back  270 0 upsideDown"
  "l1080p60    1920 1080 60 bgra back  180 0 landscapeLeft"
)
SOURCES=(scene image video)
srcspec() {
  case $1 in
    scene) print -r -- scene;;
    image) print -r -- "image:$SIMCAM_DEV/media/qr.png";;
    video) print -r -- "video:$SIMCAM_HOME/golden/slow10.mov";;
  esac
}
case $CMD in
  capture)
    LABEL=${1:?label}; LDR=${2:-}; OUT=$SIMCAM_RESULTS/golden-$LABEL; rm -rf $OUT; mkdir -p $OUT
    if [[ ! -f $SIMCAM_HOME/golden/slow10.mov ]]; then
      mkdir -p $SIMCAM_HOME/golden; $ROOT/tools/build.sh mkvideo >/dev/null || exit 1
      $ROOT/tools/bin/mkvideo $SIMCAM_HOME/golden/slow10.mov 10 30 >/dev/null || exit 1
    fi
    for rt in ${=RTS:-263 27}; do
      need_udid "$0" "$rt" U short
      PREV=$(cat $SIMCAM_HOME/sims/$U/current 2>/dev/null)
      [[ -n $LDR ]] && $ROOT/scripts/swap.sh $U $LDR >/dev/null
      CUR=$(<$SIMCAM_HOME/sims/$U/current); CUR=${CUR#1 }; echo "== $rt: engine ${CUR:h:t}"
      xcrun simctl privacy $U grant camera $B 2>/dev/null
      for src in $SOURCES; do for c in $CASES; do
        set -- ${=c}; NAME=$src-$1; W=$2; H=$3; FPS=$4; FMT=$5; POS=$6; ROT=$7; MIR=$8; OR=$9
        SECS=8; [[ $src == image ]] && SECS=3
        CTL=$OUT/$rt-$NAME.control; print -r -- "source=$(srcspec $src)"$'\n'"orientation=$OR" > $CTL
        typeset -a ENVS; ENVS=(SIMCTL_CHILD_M5APP_MODE=golden "SIMCTL_CHILD_SIMCAM_CONTROL=$CTL" SIMCTL_CHILD_GF_W=$W SIMCTL_CHILD_GF_H=$H
          SIMCTL_CHILD_GF_FPS=$FPS SIMCTL_CHILD_GF_FMT=$FMT SIMCTL_CHILD_GF_POS=$POS SIMCTL_CHILD_GF_ROT=$ROT SIMCTL_CHILD_GF_MIRROR=$MIR
          SIMCTL_CHILD_GF_SECS=$SECS SIMCTL_CHILD_GF_PHOTO=$([[ $src == image ]] && echo 1 || echo 0) SIMCTL_CHILD_GF_MOVIE=$([[ $src == image ]] && echo 2 || echo 0))
        LOG=$OUT/$rt-$NAME.log
        xcrun simctl terminate $U $B 2>/dev/null
        env $ENVS xcrun simctl launch --console-pty $U $B > $LOG 2>&1 &
        LP=$!
        wait_for_line $LOG DONE 400 $LP   # up to 40 s
        kill $LP 2>/dev/null; xcrun simctl terminate $U $B 2>/dev/null
        { grep -oE "GFH [^ ]+" $LOG | cut -c5-; grep -oE "GFPHOTO [^ ]+" $LOG | sed 's/GFPHOTO/photo/'; grep -oE "GFMOVIE [0-9]+ [^ ]+" $LOG | tr -d '\r' | sed 's/^GFMOVIE/movie/'; } | LC_ALL=C sort -u > $OUT/$rt-$NAME.txt
        printf "%-22s %s %s\n" $NAME "$(grep -oE 'GFSTAT.*|GFMOVIESTAT [^ ]+ [^ ]+' $LOG | tr -d '\r' | tr '\n' ' ')" "$(grep -cE '\| FAIL' $LOG | sed 's/^0$//;s/^[1-9].*/FAIL (see log)/')"
      done; done
      # an `if`: a false && list as the loop's last statement made the exit status 1
      if [[ -n $LDR && -n $PREV ]]; then
        $ROOT/scripts/swap.sh $U "${PREV#1 }" >/dev/null
      fi
    done;;
  compare)
    A=$SIMCAM_RESULTS/golden-${1:?labelA}; Bd=$SIMCAM_RESULTS/golden-${2:?labelB}; bad=0
    printf "%-26s %8s %8s %8s %7s %7s  %s\n" case frames-A frames-B common only-A only-B result
    for f in $A/*.txt(N); do
      n=${f:t:r}; g=$Bd/${f:t}
      [[ -f $g ]] || { printf "%-26s missing in %s\n" $n ${Bd:t}; bad=1; continue; }
      na=$(grep -vcE '^(photo|movie)' $f); nb=$(grep -vcE '^(photo|movie)' $g)
      cm=$(LC_ALL=C comm -12 $f $g | grep -vcE '^(photo|movie)'); oa=$(LC_ALL=C comm -23 $f $g | wc -l | tr -d ' ')
      ob=$(LC_ALL=C comm -13 $f $g | wc -l | tr -d ' ')
      r=identical; (( oa || ob || na == 0 )) && { r=DIFFERENT; bad=1; }
      (( na == 0 )) && r="NO FRAMES"
      printf "%-26s %8s %8s %8s %7s %7s  %s\n" $n $na $nb $cm $oa $ob "$r"
    done
    for g in $Bd/*.txt(N); do [[ -f $A/${g:t} ]] || { printf "%-26s missing in %s\n" ${g:t:r} ${A:t}; bad=1; }; done
    (( bad )) && echo "golden: differences found" || echo "golden: all cases identical"
    exit $bad;;
  *) print_usage $0; exit 2;;
esac
