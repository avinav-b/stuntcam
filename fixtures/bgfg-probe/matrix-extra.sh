#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# matrix-extra.sh <rt> <tag> — the extra appNotif behaviour: armed 3 reps with and without M12_NO_INTERRUPT=1, then 1 unarmed
set -u; ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; RT=$1; TAG=$2; D=${0:a:h}; export BGS=${BGS:-6}
$ROOT/scripts/boot.sh $RT stub || exit 1
V=SIMCAM_UDID_$RT; xcrun simctl install ${(P)V} $D/build/bgfgprobe.app || exit 1
for r in 1 2 3; do XENV= $D/drive.sh $RT appNotif $TAG-r$r boot; XENV=M12_NO_INTERRUPT=1 $D/drive.sh $RT appNotif $TAG-noint-r$r boot; done
$ROOT/scripts/boot.sh $RT plain || exit 1
XENV= $D/drive.sh $RT appNotif $TAG-r1 none
xcrun simctl shutdown ${(P)V}
echo "MATRIX DONE $RT"
