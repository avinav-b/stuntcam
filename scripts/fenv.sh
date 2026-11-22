# SPDX-License-Identifier: Apache-2.0
# Source this file to use a local Flutter SDK in this shell only (it edits no PATH or profile outside it).
#   FLUTTER_SDK must name a local Flutter SDK checkout. Caches go to build/flutter/; F is set to the flutter binary.
STUNTCAM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)"
: ${FLUTTER_SDK:?set FLUTTER_SDK to a local Flutter SDK checkout}
export PUB_CACHE=${PUB_CACHE:-$STUNTCAM_ROOT/build/flutter/pub-cache}
export XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$STUNTCAM_ROOT/build/flutter/xdg-config}
export FLUTTER_SUPPRESS_ANALYTICS=true LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 CI=true
F=$FLUTTER_SDK/bin/flutter
