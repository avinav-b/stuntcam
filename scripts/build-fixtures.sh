#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# build-fixtures.sh [--clean] [app ...] — build every test app and install it on the sims named by $SIMCAM_UDID_{263,27,175}
#   apps (default: all, in this order): canary plain plain-orient expo flutter
#   --clean   first remove each app's local deps and build outputs (node_modules, ios/Pods, build/, .dart_tool, …)
#   FLUTTER_SDK = a local Flutter SDK checkout (required for flutter; e.g. ~/flutter). Never copied into the repo.
# Deps install locally only (npm ci, pod install, flutter pub get with PUB_CACHE under build/flutter/). Build logs:
# build/fixtures/<app>.log; per-app timings in build/fixtures/times.txt. Exit status 1 if any app failed (the others still build).
# Products:
#   canary        canary/build/m10canary.app                                                (dev.simcam.spike.m10canary)
#   plain         fixtures/plain/build/m5app.app                                            (dev.simcam.spike.m10plain)
#   plain-orient  fixtures/plain-orient/build/p12.app                                       (dev.simcam.spike.m12plain)
#   expo          fixtures/expo/build/dd/Build/Products/Debug-iphonesimulator/m1bcam.app    (dev.spike.m1bcam)
#   flutter       fixtures/flutter/build/ios/iphonesimulator/Runner.app                     (dev.spike.m7cam)
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; LOGS=$ROOT/build/fixtures; mkdir -p $LOGS
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8   # CocoaPods needs a UTF-8 locale
CLEAN=0; typeset -a APPS; APPS=()
for a in "$@"; do case $a in --clean) CLEAN=1;; -h|--help) sed -n '3,16p' "$0"; exit 0;; *) APPS+=($a);; esac; done
(( ${#APPS} )) || APPS=(canary plain plain-orient expo flutter)
typeset -a UDIDS; UDIDS=(); for v in SIMCAM_UDID_263 SIMCAM_UDID_27 SIMCAM_UDID_175; do [[ -n ${(P)v:-} ]] && UDIDS+=(${(P)v}); done
(( ${#UDIDS} )) || echo "build-fixtures.sh: no SIMCAM_UDID_* set; building without installing" >&2

install_app() { local u; for u in $UDIDS; do xcrun simctl install $u $1 || return 1; echo "installed ${1:t} on $u"; done; }
rn_build() {   # rn_build <dir> [xcodebuild settings...]
  local d=$1; shift
  (( CLEAN )) && rm -rf $d/node_modules $d/ios/Pods $d/ios/build $d/build
  (cd $d && npm ci --no-audit --no-fund) || return 1
  (cd $d/ios && pod install) || return 1
  xcodebuild -workspace $d/ios/m1bcam.xcworkspace -scheme m1bcam -configuration Debug -sdk iphonesimulator \
    -destination 'generic/platform=iOS Simulator' ARCHS=arm64 -derivedDataPath $d/build/dd "$@" build || return 1
  install_app $d/build/dd/Build/Products/Debug-iphonesimulator/m1bcam.app
}
build_app() {
  case $1 in
    canary) (( CLEAN )) && rm -rf $ROOT/canary/build
            $ROOT/canary/build.sh && install_app $ROOT/canary/build/m10canary.app;;
    plain) (( CLEAN )) && rm -rf $ROOT/fixtures/plain/build
           SIMCAM_UDID_263=${SIMCAM_UDID_263:-} SIMCAM_UDID_27=${SIMCAM_UDID_27:-} SIMCAM_UDID_175=${SIMCAM_UDID_175:-} $ROOT/fixtures/plain/build.sh;;
    plain-orient) (( CLEAN )) && rm -rf $ROOT/fixtures/plain-orient/build
                  $ROOT/fixtures/plain-orient/build.sh;;   # installs on $SIMCAM_UDID_263 / $SIMCAM_UDID_27 / $SIMCAM_UDID_175
    expo) rn_build $ROOT/fixtures/expo;;
    flutter)
      [[ -n ${FLUTTER_SDK:-} && -x $FLUTTER_SDK/bin/flutter ]] || { echo "set FLUTTER_SDK to a local Flutter SDK checkout"; return 1; }
      local d=$ROOT/fixtures/flutter
      (( CLEAN )) && rm -rf $d/build $d/.dart_tool $d/ios/Pods $d/.flutter-plugins-dependencies
      source $ROOT/scripts/fenv.sh   # PUB_CACHE / XDG_CONFIG_HOME under build/flutter/, $F
      (cd $d && $F pub get && $F build ios --simulator --debug) || return 1
      install_app $d/build/ios/iphonesimulator/Runner.app;;
    *) echo "unknown app $1"; return 2;;
  esac
}

fail=0; T00=$SECONDS; : > $LOGS/times.txt
for a in $APPS; do
  t0=$SECONDS; echo "== $a (log: build/fixtures/$a.log)"
  if ( build_app $a ) > $LOGS/$a.log 2>&1; then r=OK; else r=FAILED; fail=1; tail -15 $LOGS/$a.log; fi
  echo "$a $r $(( SECONDS - t0 ))s" | tee -a $LOGS/times.txt
done
echo "total $(( SECONDS - T00 ))s" | tee -a $LOGS/times.txt
exit $fail
