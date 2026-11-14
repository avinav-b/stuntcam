#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# final.sh <rt> — exhaustive VisionCamera checks 30x (263/27 only) then a 50-launch soak
HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; mkdir -p $SIMCAM_RESULTS
[[ $RT != 175 ]] && $HERE/loop.sh $RT 30 VCF30 vcfull
$HERE/loop.sh $RT 50 SOAK canary plain vc vc-audio vcfull expo expo-audio flutter
echo FINAL DONE > $SIMCAM_RESULTS/final-done-$RT.txt
