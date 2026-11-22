#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# orient-lib.sh <rt> <test> <tag> [K=V|-Arg v ...] — runs lib.sh for back/front x portrait/landscapeLeft/
# landscapeRight and prints the orientation class of the preview, the photo and the movie of each run.
setopt nullglob
HERE=${0:A:h}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

RUNTIME=$1
TEST=$2
TAG=$3
shift 3

# orient_class <image>: its orientation and size from tools/orient.py ("-" if there is no such file).
orient_class() {
  [[ -f $1 ]] && python3 -I $ROOT/tools/orient.py $1 | sed -E 's/^PASS orient=//; s/ size=/ /' || echo "-"
}

for cam in back front; do
  for orientation in portrait landscapeLeft landscapeRight; do
    result=$($HERE/lib.sh $RUNTIME $TEST $cam $orientation $TAG "$@")
    run_dir=$SIMCAM_RESULTS/lib-$TEST-$RUNTIME-$cam-$orientation-$TAG

    # a run that saved no photo, movie or preview leaves that one empty
    photo=$(ls $run_dir/*.heic $run_dir/*.jpg $run_dir/*.jpeg 2>/dev/null | grep -v final | head -1)
    movie=$(ls $run_dir/*.mov $run_dir/*.mp4 2>/dev/null | head -1)

    [[ -n $photo ]] && $ROOT/tools/bin/upright $photo $run_dir/photo-up.png >/dev/null
    [[ -n $movie ]] && $ROOT/tools/bin/upright $movie $run_dir/movie-up.png >/dev/null

    preview=$run_dir/preview.png
    [[ -f $preview ]] || preview=$(ls $run_dir/*preview*.png 2>/dev/null | head -1)
    preview_class=$(orient_class $preview)
    photo_class=$(orient_class $run_dir/photo-up.png)
    movie_class=$(orient_class $run_dir/movie-up.png)

    echo "$RUNTIME $TEST $cam $orientation | preview $preview_class | photo $photo_class | movie $movie_class |" \
      "${result#*:: }"
  done
done
