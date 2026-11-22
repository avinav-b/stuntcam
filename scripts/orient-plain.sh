#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# orient-plain.sh <rt> <tag> — runs p12.sh's orient mode (the plain app) for back/front x portrait/landscapeLeft/
# landscapeRight and prints tools/orient.py's verdict on the preview, the photo and the movie of each run.
HERE=${0:A:h}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

RUNTIME=$1
TAG=$2
shift 2

# orient_verdict <image> <expected class>: the first 40 characters of tools/orient.py's verdict (or its error).
orient_verdict() { python3 -I $ROOT/tools/orient.py $1 $2 2>&1 | cut -c1-40; }

for cam in back front; do
  for orientation in portrait landscapeLeft landscapeRight; do
    $HERE/p12.sh $RUNTIME orient $cam $orientation $TAG "$@" >/dev/null
    run_dir=$SIMCAM_RESULTS/p12-orient-$RUNTIME-$cam-$orientation-$TAG

    # The expected class is upright, and mirrored where the app saw a mirrored connection (the front preview always).
    preview_expect=$([[ $cam == front ]] && echo 0m || echo 0)
    movie_expect=$(grep -q "movie connection.*mirrored=true" $run_dir/app.log && echo 0m || echo 0)
    photo_expect=$(grep -q "photo connection.*mirrored=true" $run_dir/app.log && echo 0m || echo 0)

    for f in photo.jpg movie.mov; do
      [[ -f $run_dir/$f ]] && $ROOT/tools/bin/upright $run_dir/$f $run_dir/${f%.*}-up.png >/dev/null
    done

    preview=$(orient_verdict $run_dir/preview.png $preview_expect)
    photo=$(orient_verdict $run_dir/photo-up.png $photo_expect)
    movie=$(orient_verdict $run_dir/movie-up.png $movie_expect)

    echo "$RUNTIME $cam $orientation | preview $preview | photo $photo | movie $movie"
  done
done
