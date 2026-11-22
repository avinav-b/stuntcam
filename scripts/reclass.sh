#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# reclass.sh <test> <tag> — classifies the preview, photo and movie of existing lib-<test>-* result dirs again
# (orient-lib.sh's runs, on 26.3 and 27).
setopt nullglob
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

TEST=$1
TAG=$2

# orient_class <image>: its orientation class (tools/orient.py), "-" if there is none.
orient_class() {
  [[ -f $1 ]] && python3 -I $ROOT/tools/orient.py $1 | sed -E 's/^(PASS|FAIL) orient=//; s/ size=.*//' || echo "-"
}

for RUNTIME in 263 27; do
  for cam in back front; do
    for orientation in portrait landscapeLeft landscapeRight; do
      run_dir=$SIMCAM_RESULTS/lib-$TEST-$RUNTIME-$cam-$orientation-$TAG
      [[ -d $run_dir ]] || continue

      photos=( $run_dir/*.heic $run_dir/*.jpg $run_dir/*.jpeg )
      movies=( $run_dir/*.mov $run_dir/*.mp4 )
      (( ${#photos} )) && $ROOT/tools/bin/upright $photos[1] $run_dir/photo-up.png >/dev/null
      (( ${#movies} )) && $ROOT/tools/bin/upright $movies[1] $run_dir/movie-up.png >/dev/null

      previews=( $run_dir/preview.png $run_dir/*preview*.png $run_dir/*picture*.png )
      printf "%-4s %-10s %-6s %-15s | preview %-4s | photo %-4s | movie %-4s | %s\n" \
        $RUNTIME $TEST $cam $orientation \
        $(orient_class $previews[1]) $(orient_class $run_dir/photo-up.png) $(orient_class $run_dir/movie-up.png) \
        "$(grep -c PASS $run_dir/app.log)P/$(grep -c FAIL $run_dir/app.log)F"
    done
  done
done
