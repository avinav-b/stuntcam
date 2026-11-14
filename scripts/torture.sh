#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# torture.sh <rt> <n> <tag> [cycles] — n launches of the RN fixture's cycle modes, alternating vccycle / expocycle, each with
#   -CYCLES <cycles> (default 10), on a sim booted by `scripts/boot.sh <rt> stub`. One line per launch (scripts/cycle.sh) in
#   results/torture-<tag>-<rt>.txt. A tag starting with MEM adds a `memcycle` line (scripts/memcycle.sh: 50 open/close cycles in
#   one plain-AVF process, footprint flat; ledger L75).
# Sanitizer runs (measured internally, W1 e3b): the TSan/ASan runtime must be in the process at launch, so the stub's
#   dlopen of a sanitizer engine only works in a sanitizer-built app. Build fixtures/expo with
#   `xcodebuild … -derivedDataPath fixtures/expo/build/dd-tsan -enableThreadSanitizer YES` (or dd-asan / -enableAddressSanitizer),
#   install it (same bundle id), `cp build/tsan_loader.txt build/current_loader.txt` (its build dir holds the TSan engine), then boot.sh. Reinstall the normal app after.
HERE=${0:A:h}; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; RT=$1; N=$2; TAG=$3; C=${4:-10}
OUTF=$SIMCAM_RESULTS/torture-$TAG-$RT.txt; : > $OUTF; T0=$(date +%s); echo $T0 > $OUTF.start
for i in {1..$N}; do m=vccycle; (( i % 2 == 0 )) && m=expocycle; $HERE/cycle.sh $RT $m $C $TAG-$i >> $OUTF 2>&1; done
# w4 e58 (L75): a MEM torture ends with the plain fixture's one-process open/close memory check (scripts/memcycle.sh)
[[ $TAG == MEM* ]] && $HERE/memcycle.sh $RT 50 >> $OUTF 2>&1
echo "TORTURE DONE $(( $(date +%s) - T0 ))s" >> $OUTF
