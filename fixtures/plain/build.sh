#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
set -e; cd ${0:a:h}
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path); APP=build/m5app.app; mkdir -p $APP
xcrun swiftc -target arm64-apple-ios17.0-simulator -sdk $SDK -O main.swift -o $APP/m5app
cat > $APP/Info.plist <<P
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>dev.simcam.spike.m10plain</string><key>CFBundleExecutable</key><string>m5app</string>
<key>CFBundleName</key><string>m5app</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string><key>LSRequiresIPhoneOS</key><true/><key>MinimumOSVersion</key><string>17.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer></array><key>UILaunchScreen</key><dict/><key>UIApplicationSceneManifest</key><dict><key>UIApplicationSupportsMultipleScenes</key><false/></dict><key>NSCameraUsageDescription</key><string>spike</string><key>NSMicrophoneUsageDescription</key><string>spike</string>
<key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string><string>UIInterfaceOrientationLandscapeLeft</string><string>UIInterfaceOrientationLandscapeRight</string></array>
</dict></plist>
P
codesign -f -s - $APP >/dev/null 2>&1
for v in SIMCAM_UDID_263 SIMCAM_UDID_27 SIMCAM_UDID_175; do [[ -n ${(P)v:-} ]] || continue; xcrun simctl install ${(P)v} $APP && echo installed $v; done
