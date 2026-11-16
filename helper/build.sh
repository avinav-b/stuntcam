#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
#
# helper/build.sh [--release] — build the camera helper, "Stuntcam Camera.app".
#
#   helper/main.swift + helper/shim.h + engine/src/webcam/ring.c, ring_shm.c  ->  build/helper/Stuntcam Camera.app (arm64)
#
# Dev build (default): bundle id dev.stuntcam.camera.dev, signed with the hardened runtime and the camera entitlement only.
# --release: bundle id dev.stuntcam.camera, left unsigned for the release workflow's sign-helper job (Developer ID,
# notarized), with build/helper/entitlements.plist next to it; not copied under $SIMCAM_HOME. Both are stripped (strip -x)
# and built with the checkout path mapped away, and the bundle is scanned for build-machine paths before it is signed.
#
# <sha> is the first 12 hex digits of the sha1 over one "<relative path> <sha1>" line per file of the signed bundle (C
# order). The bundle is copied to $SIMCAM_HOME/helper/<sha>/Stuntcam Camera.app, the path it is launched from
# (helper/dev.sh), and build/current_helper.txt names that copy. Signed with $STUNTCAM_DEV_SIGN when set (a codesigning
# identity, e.g. an Apple Development certificate: macOS then keeps the camera grant across rebuilds, since it follows
# the team and bundle id), else ad-hoc, and macOS asks for camera permission again after each rebuild that changes the
# binary. --synthetic never asks.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"; cd "$ROOT"; source $ROOT/scripts/env.sh
RELEASE=0
case ${1:-} in --release) RELEASE=1;; "") ;; *) echo "helper/build.sh: unknown argument $1" >&2; exit 2;; esac
ID=dev.stuntcam.camera.dev; (( RELEASE )) && ID=dev.stuntcam.camera
OUT=build/helper; APP="$OUT/Stuntcam Camera.app"; OBJ=$OUT/obj
rm -rf "$APP" $OBJ; mkdir -p "$APP/Contents/MacOS" $OBJ
TARGET=arm64-apple-macos14.0
for f in ring ring_shm; do
  xcrun clang -target $TARGET -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Werror -ffile-prefix-map=$ROOT/=  \
    -c engine/src/webcam/$f.c -o $OBJ/$f.o
done
xcrun swiftc -O -swift-version 5 -target $TARGET -import-objc-header helper/shim.h -Xcc -Iengine/src/webcam \
  -file-prefix-map $ROOT/= helper/main.swift $OBJ/ring.o $OBJ/ring_shm.o -o "$APP/Contents/MacOS/stuntcam-camera"
xcrun strip -x "$APP/Contents/MacOS/stuntcam-camera"
VERSION=$(cat VERSION 2>/dev/null || echo 0)
cat > "$APP/Contents/Info.plist" <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>$ID</string>
<key>CFBundleName</key><string>Stuntcam Camera</string>
<key>CFBundleDisplayName</key><string>Stuntcam Camera</string>
<key>CFBundleExecutable</key><string>stuntcam-camera</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>$VERSION</string>
<key>CFBundleVersion</key><string>$VERSION</string>
<key>LSMinimumSystemVersion</key><string>14.0</string>
<key>LSUIElement</key><true/>
<key>NSCameraUsageDescription</key><string>Stuntcam sends this camera to your iOS Simulator apps.</string>
<key>NSCameraUseContinuityCameraDeviceType</key><true/>
<key>NSCameraReactionEffectGesturesEnabledDefault</key><false/>
</dict></plist>
P
cat > $OBJ/entitlements.plist <<P
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>com.apple.security.device.camera</key><true/></dict></plist>
P
if find "$APP" -type l | grep -q .; then echo "helper/build.sh: the bundle contains a symlink" >&2; exit 1; fi
# no build-machine paths in what gets signed (grep: 0 = found, 1 = none, 2 = error)
rc=0; hits=$(LC_ALL=C grep -rlaF -e "$ROOT" -e "$HOME" -e /Users/ -e /private/var/folders -e stuntcam-work "$APP") || rc=$?
if (( rc != 1 )); then echo "helper/build.sh: build-machine paths in the bundle (grep exit $rc): $hits" >&2; exit 1; fi
if (( RELEASE )); then
  cp $OBJ/entitlements.plist $OUT/entitlements.plist
  echo "helper (unsigned, $ID): $APP; entitlements $OUT/entitlements.plist"
  exit 0
fi
codesign -f -s "${STUNTCAM_DEV_SIGN:--}" -o runtime -i $ID --entitlements $OBJ/entitlements.plist "$APP"
codesign --verify --strict "$APP"
SHA=$(cd "$APP" && LC_ALL=C find . -type f | LC_ALL=C sort | while IFS= read -r f; do print -r -- "${f#./} $(shasum < "$f" | cut -c1-40)"; done | shasum | cut -c1-12)
DEST="$SIMCAM_HOME/helper/$SHA/Stuntcam Camera.app"
if [[ ! -d $DEST ]]; then mkdir -p "${DEST:h}"; ditto "$APP" "$DEST"; fi
codesign --verify --strict "$DEST"
print -r -- "$DEST" > build/current_helper.txt
echo "helper $SHA: $DEST"
codesign -dv "$DEST" 2>&1 | grep -E '^(Identifier|CodeDirectory|Signature)' | tr '\n' ' '; echo
