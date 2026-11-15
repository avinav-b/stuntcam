#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# matrix.sh <rt> <tag> [N=20] [UNARMED=5] — the motion-probe runs on one runtime; the sim must be booted by scripts/boot.sh <rt> stub
#   N launches of each of lazy|eager x camera 0|1 x motionFirst|cameraFirst, interleaved (rep outer, variant inner), then
#   UNARMED launches with SIMCAM=0 (-camera 1 -order motionFirst). Result dirs: $SIMCAM_RESULTS/motion-<rt>-<tag>-<variant>-<rep>/
set -u; D=${0:a:h}; RT=$1; TAG=$2; N=${3:-20}; NU=${4:-5}
typeset -a V; V=()
for arm in lazy eager; do for c in 0 1; do for o in motionFirst cameraFirst; do V+=($arm:$c:$o); done; done; done
for r in {1..$N}; do
  for v in $V; do arm=${v%%:*}; rest=${v#*:}; c=${rest%%:*}; o=${rest#*:}
    x=; [[ $arm == eager ]] && x=SIMCAM_LAZY=0
    XENV=$x $D/drive.sh $RT $TAG-$arm-c$c-$o-$r boot -camera $c -order $o
  done
done
for r in {1..$NU}; do XENV=SIMCAM=0 $D/drive.sh $RT $TAG-unarmed-c1-motionFirst-$r boot -camera 1 -order motionFirst; done
echo "matrix $RT $TAG done"
