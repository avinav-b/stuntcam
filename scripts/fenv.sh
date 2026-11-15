# SPDX-License-Identifier: Apache-2.0
# source me: local Flutter only (no PATH/profile edits outside this shell)
#   FLUTTER_SDK = a local Flutter SDK checkout; caches go to build/flutter/
STUNTCAM_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]:-${(%):-%x}}")/.." && pwd)"
: ${FLUTTER_SDK:?set FLUTTER_SDK to a local Flutter SDK checkout}
export PUB_CACHE=${PUB_CACHE:-$STUNTCAM_ROOT/build/flutter/pub-cache} XDG_CONFIG_HOME=${XDG_CONFIG_HOME:-$STUNTCAM_ROOT/build/flutter/xdg-config} FLUTTER_SUPPRESS_ANALYTICS=true LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 CI=true
F=$FLUTTER_SDK/bin/flutter
