#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# tools/build.sh [name ...] — compile the helper tools in tools/ into tools/bin/ (git-ignored). No args: all of them.
#
#   macOS host tools (run on the Mac):  avaudio avinfo mkqr mkvideo upright (Swift), pstart (C)
#   iOS Simulator CLIs (run inside a sim with `xcrun simctl spawn <udid> tools/bin/<name>`):  audiosrc dumpkeys mrctest (ObjC)
#
# The spikes didn't record these commands; they are rebuilt from spike m12's binaries:
# platform, minimum OS, ARC, linked frameworks, no debug info, ad-hoc linker signature. Host tools use the SDK's default
# deployment target, as the spikes' plain `swiftc` / `clang` did. audiosrc targets iOS 17.0, dumpkeys and mrctest 26.0.
set -euo pipefail
cd "$(dirname "$0")"; mkdir -p bin
MAC=$(xcrun --sdk macosx --show-sdk-path); SIM=$(xcrun --sdk iphonesimulator --show-sdk-path)
host_swift() { xcrun -sdk macosx swiftc -O -sdk $MAC $1.swift -o bin/$1; }
host_c()     { xcrun -sdk macosx clang -O2 -isysroot $MAC $1.c -o bin/$1; }
sim_objc()   { local n=$1 min=$2; shift 2; xcrun clang -target arm64-apple-ios$min-simulator -isysroot $SIM -fobjc-arc -O2 $n.m "$@" -o bin/$n; }
build() {
  case $1 in
    avaudio|avinfo|mkqr|mkvideo|upright) host_swift $1;;
    pstart) host_c $1;;
    audiosrc) sim_objc $1 17.0 -framework Foundation -framework AVFoundation;;
    dumpkeys) sim_objc $1 26.0 -framework Foundation;;
    mrctest)  sim_objc $1 26.0 -framework Foundation -framework CoreImage;;
    *) echo "tools/build.sh: unknown tool $1" >&2; return 2;;
  esac
}
ALL=(avaudio avinfo mkqr mkvideo upright pstart audiosrc dumpkeys mrctest)
(( $# )) || set -- $ALL
fail=0
for t in "$@"; do if build $t; then echo "built tools/bin/$t"; else echo "FAILED tools/bin/$t" >&2; fail=1; fi; done
exit $fail
