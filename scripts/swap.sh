#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# swap.sh <udid> [loader path] — point an armed sim at another build without rebooting (design §4.1, U14). The frozen stub
# reads $SIMCAM_HOME/sims/<udid>/current at every app launch, so the next launch uses the new loader + engine pair.
#   default loader: build/current_loader.txt (the last ./build.sh). Running apps keep the build they started with.
#   A sim with no config.json (armed before it existed) gets one with the Xcode in use, as `stuntcam swap` does; an
#   existing one is pointed at this clone's staged restore rules ($SIMCAM_RULES_DIR), keeping the recorded Xcode.
set -u; ROOT="$(cd "$(dirname "$0")/.." && pwd)"; source $ROOT/scripts/env.sh; source $ROOT/scripts/arm-env.sh
UDID=${1:-}; [[ -n $UDID ]] || { echo "usage: $0 <udid> [loader path]" >&2; exit 2; }
LDR=${2:-$(cat $ROOT/build/current_loader.txt)}
[[ -f $LDR ]] || { echo "$0: no loader at $LDR" >&2; exit 1; }
[[ -f ${LDR:h}/libsimcam.dylib ]] || echo "$0: warning: no libsimcam.dylib next to $LDR (the loader loads that one unless SIMCAM_DYLIB is set)" >&2
simcam_guard $LDR
D=$SIMCAM_HOME/sims/$UDID; mkdir -p "$D"
print -r -- "1 $LDR" > "$D/current.tmp" && mv "$D/current.tmp" "$D/current"   # atomic: a launch never reads half a line
if [[ -f $D/config.json ]]; then simcam_set_rules_dir "$D"   # this build's restore rules apply from the next launch too
else simcam_record_xcode "$D" swap; fi
echo "sim $UDID now uses ${LDR:h:t} ($LDR)"
