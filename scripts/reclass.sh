#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# reclass.sh <test> <tag> — classify preview/photo/movie of existing lib-<test>-* result dirs
setopt nullglob; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; T=$1; TAG=$2
c() { [[ -f $1 ]] && python3 -I $ROOT/tools/orient.py $1 | sed -E 's/^(PASS|FAIL) orient=//; s/ size=.*//' || echo "-"; }
for RT in 263 27; do for cam in back front; do for o in portrait landscapeLeft landscapeRight; do
  D=$SIMCAM_RESULTS/lib-$T-$RT-$cam-$o-$TAG; [[ -d $D ]] || continue
  ph=( $D/*.heic $D/*.jpg $D/*.jpeg ); mv=( $D/*.mov $D/*.mp4 )
  (( ${#ph} )) && $ROOT/tools/bin/upright $ph[1] $D/photo-up.png >/dev/null; (( ${#mv} )) && $ROOT/tools/bin/upright $mv[1] $D/movie-up.png >/dev/null
  pv=( $D/preview.png $D/*preview*.png $D/*picture*.png )
  printf "%-4s %-10s %-6s %-15s | preview %-4s | photo %-4s | movie %-4s | %s\n" $RT $T $cam $o $(c $pv[1]) $(c $D/photo-up.png) $(c $D/movie-up.png) "$(grep -c PASS $D/app.log)P/$(grep -c FAIL $D/app.log)F"
done; done; done
