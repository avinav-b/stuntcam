#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# boot.sh <rt> stub|launchd|plain [udid] — (re)boot our own sim for runtime <rt>, armed or not
#   stub     boot with the boot-env stub: the variables in scripts/arm-env.sh go to `simctl boot` as SIMCTL_CHILD_<VAR>
#   launchd  boot plainly, then arm in launchd mode (scripts/launchd-arm.sh: the same variables via `launchctl setenv` in the
#            sim, our stub appended to any DYLD_INSERT_LIBRARIES already there; design §4.2). Apps launched afterwards are armed.
#   plain    boot without anything (exported SIMCTL_CHILD_* are dropped from the boot env too)
#   SIMCAM_CHAIN_FILE overrides the chain file (default $SIMCAM_HOME/sims/<udid>/chain.txt).
#   Everything the sim reads or writes is under $SIMCAM_HOME (scripts/env.sh); a warning names any path that isn't reachable.
#   UDID: 3rd argument, else $SIMCAM_UDID_<rt> (e.g. SIMCAM_UDID_263). Run ./build.sh first (writes build/current_*.txt).
#   Extra settings (stub and launchd): export SIMCTL_CHILD_<VAR> before calling (e.g. SIMCTL_CHILD_SIMCAM_LAZY=0 arms at load).
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh
source $ROOT/scripts/arm-env.sh; RT=$1; MODE=$2; UDID=${3:-}
[[ $MODE == (stub|launchd|plain) ]] || { echo "$0: mode must be stub, launchd or plain" >&2; exit 2; }
[[ -z $UDID ]] && need_udid "$0" "$RT" UDID long
mkdir -p $ROOT/build
xcrun simctl shutdown $UDID 2>/dev/null; for i in {1..30}; do xcrun simctl list devices | grep $UDID | grep -q Shutdown && break; sleep 1; done
if [[ $MODE == stub ]]; then
  simcam_arm_vars $RT $UDID
  env "${(@)ARMV/#/SIMCTL_CHILD_}" xcrun simctl boot $UDID || { echo "$0: simctl boot failed" >&2; exit 1; }
else
  # launchd mode sets the SIMCTL_CHILD_ extras with launchctl setenv after the boot. A variable that is also in the boot env
  # (simctl boot passes SIMCTL_CHILD_* on by itself) never reads back there, so arming would fail and roll back:
  # boot without them in that mode. plain boots without anything, so it drops them too (and a later launchd-arm.sh can arm it)
  typeset -a UNS; UNS=(); for v in ${(f)"$(env | grep -oE '^SIMCTL_CHILD_[A-Za-z0-9_]+')"}; do [[ -n $v ]] && UNS+=(-u $v); done
  rm -f $ROOT/build/bootlogdir-$RT.txt; env $UNS xcrun simctl boot $UDID || { echo "$0: simctl boot failed" >&2; exit 1; }
fi
xcrun simctl bootstatus $UDID -b >/dev/null 2>&1 || { echo "$0: bootstatus failed" >&2; exit 1; }
if [[ $MODE == launchd ]]; then RT=$RT $ROOT/scripts/launchd-arm.sh $UDID || { echo "$0: launchd arming failed" >&2; exit 1; }; fi
echo "booted $RT ($MODE) $(cat $ROOT/build/bootlogdir-$RT.txt 2>/dev/null)"
