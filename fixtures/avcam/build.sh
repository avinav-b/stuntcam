#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# build.sh [--no-livephoto-fix] [<AVCam zip | unzipped sample dir>] — build Apple's AVCam sample, patched, for the Simulator
#   Copies the sample into fixtures/avcam/build/<variant>/src (git-ignored), applies avcam.patch, builds with xcodebuild
#   (Debug, iphonesimulator, this Mac's architecture, unsigned, minimum iOS 26.0) and prints the .app path.
#   No argument: downloads the zip from Apple (URL below) to build/AVCam.zip and checks its sha256.
#   --no-livephoto-fix  applies only part (b) of the patch (the Xcode 27.0 build fix), not the Live Photo fix: the sample as
#                       Apple ships it, which crashes at the shutter under stuntcam. Variant dir: build/control/ (else
#                       build/patched/).
#   With the iOS 27.1 SDK or later, AVCAM_SDK_27_1 is set, so part (b) compiles Apple's code unchanged.
set -euo pipefail
D=${0:A:h}; B=$D/build
URL=https://docs-assets.developer.apple.com/published/cd906d1738b2/AVCamBuildingACameraApp.zip
SHA=5a1981f90bb30827121324d4b33198d58de9b9d68e3508b64f0aabc18bbc0e53
FIX=1; IN=; OWN=0
for a in "$@"; do
  case $a in
    --no-livephoto-fix) FIX=0;;
    -h|--help) sed -n '3,10p' "$0"; exit 0;;
    -*) echo "build.sh: unknown option $a" >&2; exit 2;;
    *) [[ -z $IN ]] || { echo "build.sh: one sample path only (got $IN and $a)" >&2; exit 2; }; IN=${a:A}; OWN=1;;
  esac
done
V=patched; [[ $FIX == 0 ]] && V=control
mkdir -p $B
if [[ -z $IN ]]; then
  IN=$B/AVCam.zip
  [[ -f $IN ]] || { echo "downloading $URL"; curl -fsSL -o $IN.part $URL && mv $IN.part $IN; }
fi
[[ -e $IN ]] || { echo "build.sh: $IN not found" >&2; exit 2; }

rm -rf $B/$V/src $B/$V/unzip; mkdir -p $B/$V
if [[ -f $IN ]]; then
  GOT=$(shasum -a 256 $IN | cut -d' ' -f1)
  if [[ $GOT != $SHA ]]; then
    # building the sample runs its build phases: a zip we downloaded must match; a zip you give us may be another version
    if (( ! OWN )); then rm -f $IN; echo "build.sh: the downloaded zip has sha256 $GOT, not $SHA; deleted it, not building" >&2; exit 1; fi
    echo "warning: $IN has sha256 $GOT, not $SHA (the version this patch was made for); trying anyway" >&2
  fi
  mkdir -p $B/$V/unzip; unzip -q $IN -d $B/$V/unzip; IN=$B/$V/unzip
fi
# the sample is the dir that holds AVCam.xcodeproj: the zip's top level, or one level down (a browser may add a folder)
P=$(find $IN -maxdepth 2 -name AVCam.xcodeproj -type d | head -1)
[[ -n $P ]] || { echo "build.sh: no AVCam.xcodeproj in $IN" >&2; exit 2; }
rsync -a --exclude .git ${P:h}/ $B/$V/src/; rm -rf $B/$V/unzip

# avcam.patch; --no-livephoto-fix keeps only the CameraView.swift section (part b)
if [[ $FIX == 1 ]]; then cp $D/avcam.patch $B/$V/applied.patch
else awk '/^--- /{keep = ($2 == "a/AVCam/CameraView.swift")} keep' $D/avcam.patch > $B/$V/applied.patch; fi
(cd $B/$V/src && patch -p1 -N -F0 -i $B/$V/applied.patch) || { echo "build.sh: the patch didn't apply (another AVCam version?)" >&2; exit 1; }

SDKV=$(xcrun --sdk iphonesimulator --show-sdk-version)
COND=(); [[ $(printf '%s\n27.1\n' $SDKV | sort -V | head -1) == 27.1 ]] && COND=('SWIFT_ACTIVE_COMPILATION_CONDITIONS=$(inherited) AVCAM_SDK_27_1')
echo "building $V (iOS Simulator SDK $SDKV) ... log: $B/$V/build.log"
(cd $B/$V/src && xcodebuild -project AVCam.xcodeproj -scheme AVCam -configuration Debug -sdk iphonesimulator \
   -destination 'generic/platform=iOS Simulator' -derivedDataPath $B/$V/dd ARCHS=$(uname -m) \
   CODE_SIGNING_ALLOWED=NO IPHONEOS_DEPLOYMENT_TARGET=26.0 "${COND[@]}" build) > $B/$V/build.log 2>&1 \
  || { grep -E 'error:' $B/$V/build.log | sort -u | head -20 >&2; echo "build.sh: xcodebuild failed (log: $B/$V/build.log)" >&2; exit 1; }
APP=$B/$V/dd/Build/Products/Debug-iphonesimulator/AVCam.app
echo "bundle id: $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' $APP/Info.plist)"
echo "app: $APP"
