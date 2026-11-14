#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# build.sh [tsan|asan] — plain12 test app (dev.simcam.spike.m12plain), installs into the sims in $SIMCAM_UDID_263 / $SIMCAM_UDID_27 / $SIMCAM_UDID_175
set -e; cd ${0:a:h}
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path); V=${1:-}; APP=build/p12$V.app; rm -rf $APP; mkdir -p $APP
SAN=(); [[ $V == tsan ]] && SAN=(-sanitize=thread); [[ $V == asan ]] && SAN=(-sanitize=address)
xcrun swiftc -target arm64-apple-ios17.0-simulator -sdk $SDK -Onone -g $SAN main.swift -o $APP/p12
BID=dev.simcam.spike.m12plain$V
cat > $APP/Info.plist <<P
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>$BID</string><key>CFBundleExecutable</key><string>p12</string>
<key>CFBundleName</key><string>p12$V</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string><key>LSRequiresIPhoneOS</key><true/><key>MinimumOSVersion</key><string>17.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer></array><key>UILaunchScreen</key><dict/><key>UIApplicationSceneManifest</key><dict><key>UIApplicationSupportsMultipleScenes</key><false/></dict><key>NSCameraUsageDescription</key><string>spike</string><key>NSMicrophoneUsageDescription</key><string>spike</string>
<key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
</dict></plist>
P
# sanitizer runtimes must ship inside the app (dyld finds them via @rpath / @executable_path/Frameworks)
if [[ -n $V ]]; then mkdir -p $APP/Frameworks; for l in $(otool -L $APP/p12 | grep -oE '@rpath/libclang_rt[^ ]+'); do f=$(find $(dirname $(xcrun -f swiftc))/../lib $(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang -name ${l#@rpath/} 2>/dev/null | head -1); cp $f $APP/Frameworks/; done
  install_name_tool -add_rpath @executable_path/Frameworks $APP/p12 2>/dev/null || true; codesign -f -s - $APP/Frameworks/*.dylib; fi
codesign -f -s - $APP >/dev/null 2>&1
for v in SIMCAM_UDID_263 SIMCAM_UDID_27 SIMCAM_UDID_175; do [[ -n ${(P)v:-} ]] || continue; xcrun simctl install ${(P)v} $APP && echo installed $BID $v; done
