#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# torture.sh <rt> <n> <tag> [cycles] — n launches of the RN fixture's cycle modes, alternating vccycle / expocycle,
#   each with -CYCLES <cycles> (default 10), on a sim booted by `scripts/boot.sh <rt> stub`. One line per launch
#   (scripts/cycle.sh) in results/torture-<tag>-<rt>.txt. A tag starting with MEM adds a `memcycle` line
#   (scripts/memcycle.sh: 50 open/close cycles in one plain-AVF process, footprint flat).
# Sanitizer runs: the TSan/ASan runtime must be in the process at launch, so the stub's
#   dlopen of a sanitizer engine only works in a sanitizer-built app. Build fixtures/expo with
#   `xcodebuild … -derivedDataPath fixtures/expo/build/dd-tsan -enableThreadSanitizer YES` (or dd-asan /
#   -enableAddressSanitizer), install it (same bundle id), `cp build/tsan_loader.txt build/current_loader.txt` (its
#   build dir holds the TSan engine), then boot.sh. Reinstall the normal app after.

HERE=${0:A:h}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"

RUNTIME=$1
N=$2
TAG=$3
CYCLES=${4:-10}

OUT_FILE=$SIMCAM_RESULTS/torture-$TAG-$RUNTIME.txt
: > $OUT_FILE

START_S=$(date +%s)
echo $START_S > $OUT_FILE.start

for i in {1..$N}; do
  mode=vccycle
  (( i % 2 == 0 )) && mode=expocycle
  $HERE/cycle.sh $RUNTIME $mode $CYCLES $TAG-$i >> $OUT_FILE 2>&1
done

[[ $TAG == MEM* ]] && $HERE/memcycle.sh $RUNTIME 50 >> $OUT_FILE 2>&1
echo "TORTURE DONE $(( $(date +%s) - START_S ))s" >> $OUT_FILE
