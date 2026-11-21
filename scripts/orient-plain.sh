#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# orient-plain.sh <rt> <tag> — plain app, back+front x portrait/landscapeLeft/landscapeRight; preview/photo/movie classified
HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; TAG=$2; shift 2
for cam in back front; do for o in portrait landscapeLeft landscapeRight; do
  $HERE/p12.sh $RT orient $cam $o $TAG "$@" >/dev/null; D=$SIMCAM_RESULTS/p12-orient-$RT-$cam-$o-$TAG
  pm=$([[ $cam == front ]] && echo 0m || echo 0)
  mm=$(grep -q "movie connection.*mirrored=true" $D/app.log && echo 0m || echo 0)
  phm=$(grep -q "photo connection.*mirrored=true" $D/app.log && echo 0m || echo 0)
  for f in photo.jpg movie.mov; do [[ -f $D/$f ]] && $ROOT/tools/bin/upright $D/$f $D/${f%.*}-up.png >/dev/null; done
  echo "$RT $cam $o | preview $(python3 -I $ROOT/tools/orient.py $D/preview.png $pm 2>&1|cut -c1-40) | photo $(python3 -I $ROOT/tools/orient.py $D/photo-up.png $phm 2>&1|cut -c1-40) | movie $(python3 -I $ROOT/tools/orient.py $D/movie-up.png $mm 2>&1|cut -c1-40)"
done; done
