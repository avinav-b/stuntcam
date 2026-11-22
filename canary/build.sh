#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# build.sh — builds the canary (iOS 27 SDK, minos 17, UIScene adopted) into build/m10canary.app; it does not
# install it (see README.md)
set -e
cd ${0:a:h}

SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
# m10canary: a historical name; the product and its bundle id keep it because scripts and the CLI use it
APP=build/m10canary.app
rm -rf $APP
mkdir -p $APP

# main.swift is the entry point. The rest is the Canary class, one file per concern (see README.md).
# -wmo makes the files one optimization unit, as when the canary was a single file.
SOURCES=(main.swift Config.swift Canary.swift Report.swift Support.swift Delegates.swift Frames.swift Discovery.swift
         MainSession.swift Photo.swift Retry.swift Trace.swift DeviceControls.swift Movie.swift Teardown.swift
         FrontMirror.swift Audio.swift)
xcrun swiftc -target arm64-apple-ios17.0-simulator -sdk $SDK -O -wmo -swift-version 5 $SOURCES -o $APP/m10canary

cat > $APP/Info.plist <<P
<?xml version="1.0" encoding="UTF-8"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>dev.simcam.spike.m10canary</string><key>CFBundleExecutable</key><string>m10canary</string>
<key>CFBundleName</key><string>m10canary</string><key>CFBundlePackageType</key><string>APPL</string><key>CFBundleVersion</key><string>1</string>
<key>CFBundleShortVersionString</key><string>1.0</string><key>LSRequiresIPhoneOS</key><true/><key>MinimumOSVersion</key><string>17.0</string>
<key>UIDeviceFamily</key><array><integer>1</integer></array><key>UILaunchScreen</key><dict/><key>NSCameraUsageDescription</key><string>canary</string>
<key>NSMicrophoneUsageDescription</key><string>canary</string>
<key>UIApplicationSceneManifest</key><dict><key>UIApplicationSupportsMultipleScenes</key><false/><key>UISceneConfigurations</key><dict/></dict>
<key>UISupportedInterfaceOrientations</key><array><string>UIInterfaceOrientationPortrait</string></array>
</dict></plist>
P

# an ad-hoc signature: the install below fails if it is missing
codesign -f -s - $APP >/dev/null 2>&1
echo built
