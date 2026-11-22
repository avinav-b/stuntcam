#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# tools/build.sh [name ...] — compile the helper tools in tools/ into tools/bin/ (git-ignored). No args: all of them.
#
#   macOS host tools (run on the host): avaudio avinfo mkqr mkvideo upright (Swift), pstart (C)
#   iOS Simulator CLIs (run inside a sim with `xcrun simctl spawn <udid> tools/bin/<name>`):
#     audiosrc dumpkeys mrctest (ObjC)
#
# The build settings below set each tool's platform, minimum OS, ARC and linked frameworks; there is no debug info,
# and the linker signs ad hoc. Host tools use the SDK's default deployment target (plain `swiftc` / `clang`).
# audiosrc targets iOS 17.0, dumpkeys and mrctest 26.0.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin

MAC=$(xcrun --sdk macosx --show-sdk-path)
SIM=$(xcrun --sdk iphonesimulator --show-sdk-path)

# host_swift <name>, host_c <name>: build <name>.swift / <name>.c into a macOS tool.
host_swift() { xcrun -sdk macosx swiftc -O -sdk $MAC $1.swift -o bin/$1; }
host_c()     { xcrun -sdk macosx clang -O2 -isysroot $MAC $1.c -o bin/$1; }

# sim_objc <name> <minimum iOS> <link args...>: builds <name>.m into an iOS Simulator command-line tool.
sim_objc() {
  local name=$1 min_ios=$2
  shift 2
  xcrun clang -target arm64-apple-ios$min_ios-simulator -isysroot $SIM -fobjc-arc -O2 $name.m "$@" -o bin/$name
}

# build <tool>: builds one tool with its settings.
build() {
  case $1 in
    avaudio|avinfo|mkqr|mkvideo|upright)
      host_swift $1
      ;;
    pstart)
      host_c $1
      ;;
    audiosrc)
      sim_objc $1 17.0 -framework Foundation -framework AVFoundation
      ;;
    dumpkeys)
      sim_objc $1 26.0 -framework Foundation
      ;;
    mrctest)
      sim_objc $1 26.0 -framework Foundation -framework CoreImage
      ;;
    *)
      echo "tools/build.sh: unknown tool $1" >&2
      return 2
      ;;
  esac
}

ALL=(avaudio avinfo mkqr mkvideo upright pstart audiosrc dumpkeys mrctest)
(( $# )) || set -- $ALL

fail=0
for tool in "$@"; do
  if build $tool; then
    echo "built tools/bin/$tool"
  else
    echo "FAILED tools/bin/$tool" >&2
    fail=1
  fi
done
exit $fail
