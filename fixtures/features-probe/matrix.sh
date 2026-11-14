#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# matrix.sh <rt: 263|27> <tag> — e22's armed runs on one runtime: the read-out ×2, then every feature launch ×2 (drive.sh).
#   The sim must be booted by scripts/boot.sh <rt> stub and have the probe installed (build.sh). One launch per feature.
#   Features: those that read YES on stuntcam (shuttersound, bracket, flash, maxres, multicam, fps), Live Photo and depth on
#   the cameras a real iPhone has them on (they read NO), AVCam's own configuration (avcam; avcam with NOCATCH=1 lets its
#   exception go uncaught, as in the sample), and two controls (baseline, moviebaseline).
set -u; D=${0:a:h}; RT=$1; TAG=$2
RUNS=(baseline:back-wide
      livephoto:back-wide livephoto:back-triple livephoto:front-truedepth
      avcam:back-wide avcam:back-triple avcam-nocatch:back-wide
      depth:back-wide depth:back-dualwide depth:back-triple depth:front-truedepth depth:back-lidar
      shuttersound:back-wide bracket:back-wide flash:back-wide flash:front-wide maxres:back-wide
      multicam:back-wide moviebaseline:back-wide fps:back-wide fps:front-wide)
for n in 1 2; do $D/drive.sh $RT boot $TAG$n read; done
for n in 1 2; do for r in $RUNS; do
  f=${r%%:*}; c=${r#*:}; nc=0; [[ $f == *-nocatch ]] && { f=${f%-nocatch}; nc=1; }
  NOCATCH=$nc $D/drive.sh $RT boot $TAG$n feature $f $c
done; done
echo "matrix $RT done"
