#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# matrix.sh <rt> <inj> <tag> — every app once on one runtime; appends to results/matrix-<inj>-<tag>.txt
HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; INJ=$2; TAG=$3; mkdir -p $SIMCAM_RESULTS
for a in canary plain vc vc-audio vcfull vcfull-audio expo expo-audio flutter; do
  $HERE/run.sh $a $RT $INJ $TAG >> $SIMCAM_RESULTS/matrix-$INJ-$TAG-$RT.txt 2>&1
done
echo "MATRIX DONE $RT" >> $SIMCAM_RESULTS/matrix-$INJ-$TAG-$RT.txt
