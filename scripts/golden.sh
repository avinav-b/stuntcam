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
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/common.sh"

CMD=${1:?capture|compare}
shift
SCRIPT=$0   # for messages: inside a function, $0 is the function's name

# fixtures/plain's bundle id. Bundle ids, the fixture's M5APP_* and GF_* inputs and its GF* log tokens are names from
# the project's early milestones (m10, m5, "golden frames"); they are data, so they keep those names.
BUNDLE=dev.simcam.spike.m10plain

# Each case: name W H fps fmt pos rot mirror orientation (the source comes from SOURCES). A name is p, l or u for the
# orientation (f: the front camera), the height (4k: 2160), then the rest: v/f for 420v/420f (no suffix, or bgra:
# BGRA), mir for mirrored, p60 for 60 fps. The rotation is not in the name.
CASES=(
  "p1080bgra   1920 1080 30 bgra back  90  0 portrait"
  "p1080v      1920 1080 30 420v back  0   0 portrait"
  "l4kv        3840 2160 30 420v back  0   0 landscapeRight"
  "f1080mir    1920 1080 30 bgra front 90  1 portrait"
  "u1440f      1920 1440 30 420f back  270 0 upsideDown"
  "l1080p60    1920 1080 60 bgra back  180 0 landscapeLeft"
)

SOURCES=(scene image video)

# srcspec <source>: the control file's source= value for that source.
srcspec() {
  case $1 in
    scene)
      print -r -- scene
      ;;
    image)
      print -r -- "image:$SIMCAM_DEV/media/qr.png"
      ;;
    video)
      print -r -- "video:$SIMCAM_HOME/golden/slow10.mov"
      ;;
  esac
}

# capture_case <out dir> <rt> <udid> <source> <case spec>: one launch of fixtures/plain in golden mode for that source
# and CASES entry. Writes <out dir>/<rt>-<source>-<case>.{control,log,txt} (.txt: the sorted frame, photo and movie
# hashes) and prints the case's line: its GFSTAT/GFMOVIESTAT counts, and "FAIL (see log)" if the app logged a FAIL.
capture_case() {
  local out=$1
  local rt=$2
  local udid=$3
  local src=$4
  local -a fields
  fields=(${=5})
  local case_name=$src-${fields[1]}
  local width=${fields[2]}
  local height=${fields[3]}
  local fps=${fields[4]}
  local pixel_format=${fields[5]}
  local position=${fields[6]}
  local rotation=${fields[7]}
  local mirror=${fields[8]}
  local orientation=${fields[9]}

  # the image source's picture is static: 3 s see all of it, and only it takes a photo and a 2 s movie
  local secs=8
  local photo=0
  local movie_secs=0
  if [[ $src == image ]]; then
    secs=3
    photo=1
    movie_secs=2
  fi

  local control=$out/$rt-$case_name.control
  print -r -- "source=$(srcspec $src)"$'\n'"orientation=$orientation" > $control

  local -a launch_env
  launch_env=(SIMCTL_CHILD_M5APP_MODE=golden "SIMCTL_CHILD_SIMCAM_CONTROL=$control"
    SIMCTL_CHILD_GF_W=$width SIMCTL_CHILD_GF_H=$height SIMCTL_CHILD_GF_FPS=$fps SIMCTL_CHILD_GF_FMT=$pixel_format
    SIMCTL_CHILD_GF_POS=$position SIMCTL_CHILD_GF_ROT=$rotation
    SIMCTL_CHILD_GF_MIRROR=$mirror SIMCTL_CHILD_GF_SECS=$secs
    SIMCTL_CHILD_GF_PHOTO=$photo
    SIMCTL_CHILD_GF_MOVIE=$movie_secs)

  local log=$out/$rt-$case_name.log
  # not running: nothing to report
  xcrun simctl terminate $udid $BUNDLE 2>/dev/null
  env $launch_env xcrun simctl launch --console-pty $udid $BUNDLE > $log 2>&1 &
  local launch_pid=$!
  wait_for_line $log DONE 400 $launch_pid   # up to 40 s
  # whatever already ended: nothing to report
  kill $launch_pid 2>/dev/null
  xcrun simctl terminate $udid $BUNDLE 2>/dev/null

  # the frame hashes (GFH <hash>-<WxH> <count>, without the count), the photo's and the movie's, sorted
  {
    grep -oE "GFH [^ ]+" $log | cut -c5-
    grep -oE "GFPHOTO [^ ]+" $log | sed 's/GFPHOTO/photo/'
    grep -oE "GFMOVIE [0-9]+ [^ ]+" $log | tr -d '\r' | sed 's/^GFMOVIE/movie/'
  } | LC_ALL=C sort -u > $out/$rt-$case_name.txt

  printf "%-22s %s %s\n" $case_name \
    "$(grep -oE 'GFSTAT.*|GFMOVIESTAT [^ ]+ [^ ]+' $log | tr -d '\r' | tr '\n' ' ')" \
    "$(grep -cE '\| FAIL' $log | sed 's/^0$//;s/^[1-9].*/FAIL (see log)/')"
}

# capture_runtime <out dir> <rt> <loader>: every source and case on the sim $SIMCAM_UDID_<rt>, with <loader> swapped
# in for the capture if it is set, and the sim's previous build swapped back afterwards.
capture_runtime() {
  local out=$1
  local rt=$2
  local loader=$3
  local udid previous current src spec
  need_udid "$SCRIPT" "$rt" udid short
  # no current yet: nothing to put back at the end
  previous=$(cat $SIMCAM_HOME/sims/$udid/current 2>/dev/null)
  [[ -n $loader ]] && $ROOT/scripts/swap.sh $udid $loader >/dev/null
  current=$(<$SIMCAM_HOME/sims/$udid/current)
  current=${current#1 }
  echo "== $rt: engine ${current:h:t}"

  # already granted: nothing to report
  xcrun simctl privacy $udid grant camera $BUNDLE 2>/dev/null
  for src in $SOURCES; do
    for spec in $CASES; do
      capture_case $out $rt $udid $src $spec
    done
  done
  # An `if`: a false && list as the function's last statement would make its exit status 1.
  if [[ -n $loader && -n $previous ]]; then
    $ROOT/scripts/swap.sh $udid "${previous#1 }" >/dev/null
  fi
}

case $CMD in
  capture)
    LABEL=${1:?label}
    LOADER=${2:-}
    OUT=$SIMCAM_RESULTS/golden-$LABEL
    rm -rf $OUT
    mkdir -p $OUT

    if [[ ! -f $SIMCAM_HOME/golden/slow10.mov ]]; then
      mkdir -p $SIMCAM_HOME/golden
      $ROOT/tools/build.sh mkvideo >/dev/null || exit 1
      $ROOT/tools/bin/mkvideo $SIMCAM_HOME/golden/slow10.mov 10 30 >/dev/null || exit 1
    fi

    for rt in ${=RTS:-263 27}; do
      capture_runtime $OUT $rt "$LOADER"
    done
    ;;
  compare)
    DIR_A=$SIMCAM_RESULTS/golden-${1:?labelA}
    DIR_B=$SIMCAM_RESULTS/golden-${2:?labelB}
    bad=0

    printf "%-26s %8s %8s %8s %7s %7s  %s\n" case frames-A frames-B common only-A only-B result
    for file_a in $DIR_A/*.txt(N); do
      name=${file_a:t:r}
      file_b=$DIR_B/${file_a:t}
      if [[ ! -f $file_b ]]; then
        printf "%-26s missing in %s\n" $name ${DIR_B:t}
        bad=1
        continue
      fi

      # Frame counts leave out the photo and movie lines.
      frames_a=$(grep -vcE '^(photo|movie)' $file_a)
      frames_b=$(grep -vcE '^(photo|movie)' $file_b)
      common=$(LC_ALL=C comm -12 $file_a $file_b | grep -vcE '^(photo|movie)')
      only_a=$(LC_ALL=C comm -23 $file_a $file_b | wc -l | tr -d ' ')
      only_b=$(LC_ALL=C comm -13 $file_a $file_b | wc -l | tr -d ' ')

      result=identical
      if (( only_a || only_b || frames_a == 0 )); then
        result=DIFFERENT
        bad=1
      fi
      (( frames_a == 0 )) && result="NO FRAMES"
      printf "%-26s %8s %8s %8s %7s %7s  %s\n" $name $frames_a $frames_b $common $only_a $only_b "$result"
    done

    for file_b in $DIR_B/*.txt(N); do
      if [[ ! -f $DIR_A/${file_b:t} ]]; then
        printf "%-26s missing in %s\n" ${file_b:t:r} ${DIR_A:t}
        bad=1
      fi
    done

    if (( bad )); then
      echo "golden: differences found"
    else
      echo "golden: all cases identical"
    fi
    exit $bad
    ;;
  *)
    print_usage $0
    exit 2
    ;;
esac
