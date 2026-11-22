#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# loop.sh <rt> <n> <tag> <app...> — n run.sh launches in boot-stub mode, cycling through the given apps. The results
# go to results/loop-<tag>-<rt>.txt.

HERE=${0:A:h}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

RUNTIME=$1
N=$2
TAG=$3
shift 3
APPS=($@)

mkdir -p $SIMCAM_RESULTS
OUT_FILE=$SIMCAM_RESULTS/loop-$TAG-$RUNTIME.txt
: > $OUT_FILE
date +%s > $SIMCAM_RESULTS/loop-$TAG-$RUNTIME.start

for i in {1..$N}; do
  app=${APPS[$(( (i - 1) % ${#APPS} + 1 ))]}
  # No canary retry in a loop: a soak counts the first verdict, so an intermittent INCONCLUSIVE stays visible.
  SIMCAM_NO_RETRY=1 QUIET=1 $HERE/run.sh $app $RUNTIME boot $TAG-$i >> $OUT_FILE 2>&1

  # Keeps disk use down by dropping the screenshots of passing runs. A run that never ended (end=0) is not a pass.
  run_dir=$SIMCAM_RESULTS/$app-$RUNTIME-boot-$TAG-$i
  grep -qE 'fail=0 end=[1-9]|core=PASS' $run_dir/summary.txt && rm -f $run_dir/*.png(N)
done

echo "LOOP DONE $RUNTIME" >> $OUT_FILE
