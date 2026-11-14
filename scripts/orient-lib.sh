#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# orient-lib.sh <rt> <test> <tag> [K=V|-Arg v ...] — back/front x portrait/LL/LR; prints the classification of preview/photo/movie
setopt nullglob; HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; T=$2; TAG=$3; shift 3
for cam in back front; do for o in portrait landscapeLeft landscapeRight; do
  r=$($HERE/lib.sh $RT $T $cam $o $TAG "$@"); D=$SIMCAM_RESULTS/lib-$T-$RT-$cam-$o-$TAG
  ph=$(ls $D/*.heic $D/*.jpg $D/*.jpeg 2>/dev/null | grep -v final | head -1); mv=$(ls $D/*.mov $D/*.mp4 2>/dev/null | head -1)
  [[ -n $ph ]] && $ROOT/tools/bin/upright $ph $D/photo-up.png >/dev/null; [[ -n $mv ]] && $ROOT/tools/bin/upright $mv $D/movie-up.png >/dev/null
  c() { [[ -f $1 ]] && python3 -I $ROOT/tools/orient.py $1 | sed -E 's/^PASS orient=//; s/ size=/ /' || echo "-"; }
  pv=$D/preview.png; [[ -f $pv ]] || pv=$(ls $D/*preview*.png 2>/dev/null | head -1)
  echo "$RT $T $cam $o | preview $(c $pv) | photo $(c $D/photo-up.png) | movie $(c $D/movie-up.png) | ${r#*:: }"
done; done
