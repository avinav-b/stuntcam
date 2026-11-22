#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# swap.sh <udid> [loader path] — points an armed sim at another build without rebooting. The frozen stub
# reads $SIMCAM_HOME/sims/<udid>/current at every app launch, so the next launch uses the new loader + engine pair.
#   The default loader is build/current_loader.txt (the last ./build.sh). Running apps keep the build they started
#   with.
#   A sim with no config.json (armed before it existed) gets one with the Xcode in use, as `stuntcam swap` does; an
#   existing one is pointed at this clone's staged restore rules ($SIMCAM_RULES_DIR), keeping the recorded Xcode.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
source "$ROOT/scripts/arm-env.sh"

UDID=${1:-}
[[ -n $UDID ]] || { echo "usage: $0 <udid> [loader path]" >&2; exit 2; }
LOADER=${2:-$(cat $ROOT/build/current_loader.txt)}
[[ -f $LOADER ]] || { echo "$0: no loader at $LOADER" >&2; exit 1; }
[[ -f ${LOADER:h}/libsimcam.dylib ]] ||
  echo "$0: warning: no libsimcam.dylib next to $LOADER (the loader loads that one unless SIMCAM_DYLIB is set)" >&2

simcam_guard $LOADER
SIM_DIR=$SIMCAM_HOME/sims/$UDID
mkdir -p "$SIM_DIR"
# Atomic: a launch never reads half a line.
print -r -- "1 $LOADER" > "$SIM_DIR/current.tmp" && mv "$SIM_DIR/current.tmp" "$SIM_DIR/current"

if [[ -f $SIM_DIR/config.json ]]; then
  simcam_set_rules_dir "$SIM_DIR"   # This build's restore rules also apply from the next launch.
else
  simcam_record_xcode "$SIM_DIR" swap
fi

echo "sim $UDID now uses ${LOADER:h:t} ($LOADER)"
