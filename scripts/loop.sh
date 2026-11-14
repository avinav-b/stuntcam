#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# loop.sh <rt> <n> <tag> <app...> — n launches cycling the given apps (boot-stub mode); results/loop-<tag>-<rt>.txt
HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; N=$2; TAG=$3; shift 3; APPS=($@); mkdir -p $SIMCAM_RESULTS
OUTF=$SIMCAM_RESULTS/loop-$TAG-$RT.txt; : > $OUTF; date +%s > $SIMCAM_RESULTS/loop-$TAG-$RT.start
for i in {1..$N}; do a=${APPS[$(( (i - 1) % ${#APPS} + 1 ))]}
  # no canary retry in a loop: a soak counts the first verdict, so an intermittent INCONCLUSIVE stays visible
  SIMCAM_NO_RETRY=1 QUIET=1 $HERE/run.sh $a $RT boot $TAG-$i >> $OUTF 2>&1
  # keep disk use down: drop screenshots of passing runs
  d=$SIMCAM_RESULTS/$a-$RT-boot-$TAG-$i; grep -qE 'fail=0 end=[1-9]|core=PASS' $d/summary.txt && rm -f $d/*.png(N)   # a run that never ended (end=0) is not a pass
done
echo "LOOP DONE $RT" >> $OUTF
