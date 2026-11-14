#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# matrix.sh <rt> <tag> — w3 e20's runs on one runtime: armed (scripts/boot.sh <rt> stub) 3 behaviours x 3 reps, each with and
# without M12_NO_INTERRUPT=1; then the sim rebooted plainly and each behaviour once unarmed. BGS (default 6) s in the background.
set -u; ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; RT=$1; TAG=$2; D=${0:a:h}; export BGS=${BGS:-6}
$ROOT/scripts/boot.sh $RT stub || exit 1
V=SIMCAM_UDID_$RT; xcrun simctl install ${(P)V} $D/build/bgfgprobe.app || exit 1
for r in 1 2 3; do for m in stopInBackground keepRunning stopWhileInterrupted; do
  XENV= $D/drive.sh $RT $m $TAG-r$r boot
  XENV=M12_NO_INTERRUPT=1 $D/drive.sh $RT $m $TAG-noint-r$r boot
done; done
$ROOT/scripts/boot.sh $RT plain || exit 1
for m in stopInBackground keepRunning stopWhileInterrupted; do XENV= $D/drive.sh $RT $m $TAG-r1 none; done
xcrun simctl shutdown ${(P)V}
echo "MATRIX DONE $RT"
