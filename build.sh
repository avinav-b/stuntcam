#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
#
# build.sh [--tsan | --asan] — build the engine for the iOS Simulator.
#
#   engine/src/simcam.m  -> libsimcam.dylib      (+ .dSYM)
#   engine/stub/stub.c   -> libsimcamstub.dylib  (the frozen, libSystem-only boot stub; its own hash dir)
#   engine/loader/loader.c -> libsimcamloader.dylib (restore rules, chain, engine dlopen; same dir as the engine)
#   engine/runnerhook/runnerhook.m -> libsimcamrunnerhook.dylib (XCTest runner side of `stuntcam exec`; its own hash dir)
#
# Universal arm64 + x86_64, minimum iOS 17.0, ad-hoc signed by the linker.
# Each product goes to build/<12-char sha1 of the binary>/ and is copied, byte-identical
# (the linker signature stays valid), to $SIMCAM_HOME/builds/<same hash>/: simulator processes can't read a clone under
# ~/Documents & co. (TCC; see scripts/env.sh, which also stages engine/profiles/ and fixtures/media/ to $SIMCAM_HOME/dev/).
# Pointer files (read by scripts/*.sh; they name the $SIMCAM_HOME copies):
#   build/current_dylib.txt, build/current_loader.txt, build/current_stub.txt, build/current_hook.txt   last build of any variant
#   build/<variant>_dylib.txt                         last build of that variant (normal | tsan | asan)
# --tsan / --asan add -fsanitize=thread / -fsanitize=address to the library (not the stub).
# EXTRA="<flags>" appends further flags to the library compile.
set -euo pipefail
# the physical path (pwd -P): the linker writes the debug map's object paths with symlinks resolved (/tmp is
# /private/tmp), so -oso_prefix and the prefix maps must use that form or a clone under a symlinked path leaks it (and
# gets another engine hash)
ROOT="$(cd "$(dirname "$0")" && pwd -P)"; cd "$ROOT"; source $ROOT/scripts/env.sh; source $ROOT/scripts/common.sh

VARIANT=normal; SAN=()
for a in "$@"; do
  case $a in
    --tsan) VARIANT=tsan; SAN=(-fsanitize=thread);;
    --asan) VARIANT=asan; SAN=(-fsanitize=address);;
    -h|--help) print_usage "$0"; exit 0;;
    *) echo "build.sh: unknown argument: $a (expected --tsan or --asan)" >&2; exit 2;;
  esac
done
EXTRA=${EXTRA:-}

MINOS=17.0
export ZERO_AR_DATE=1   # no object timestamps in the debug map: the same source gives the same binary and hash
ARCHS=(arm64 x86_64)
SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
TMP=build/tmp-$VARIANT; rm -rf $TMP; mkdir -p $TMP

# Compiler flags: -Wl,-adhoc_codesign makes the linker sign every slice
# (it already does so by default for arm64, not for x86_64).
# -ffile-prefix-map + -oso_prefix keep the checkout's path out of the engine's debug info and debug map: the same
# source gives the same engine hash in any checkout, and the npm/tap package has no build-machine paths. The stub, loader
# and hook carry no paths, so they don't need it (the stub stays frozen).
LIB_FLAGS=(-fobjc-arc -O1 -g -Wall -Wno-unused-function -Wno-unused-variable -ffile-prefix-map=$ROOT/= -fdebug-compilation-dir=.)
STUB_FLAGS=(-O2)
FRAMEWORKS=(-framework Foundation -framework CoreMedia -framework CoreVideo -framework CoreGraphics -framework ImageIO
  -framework IOSurface -framework VideoToolbox -framework AVFoundation -framework CoreImage -framework QuartzCore
  -framework CoreMotion -framework UIKit -framework AudioToolbox -framework AVFAudio -framework Metal)
LINK_FLAGS=(-Wl,-adhoc_codesign)

# build_one <name> <source> <flags...> -- <link args...>: one thin dylib per arch, then lipo
build_one() {
  local name=$1 src=$2; shift 2
  local -a cflags largs slices; cflags=(); largs=(); slices=()
  while (( $# )) && [[ $1 != -- ]]; do cflags+=($1); shift; done; (( $# )) && shift; largs=("$@")
  for arch in $ARCHS; do
    # compile to a fixed object path, then link: clang's default temp .o name is random and lands in the debug map, which
    # changed the content hash on every build of unchanged source (and so reset sentinel latches)
    xcrun clang -target $arch-apple-ios$MINOS-simulator -isysroot $SDK $cflags -c $src -o $TMP/$name.$arch.o
    xcrun clang -target $arch-apple-ios$MINOS-simulator -isysroot $SDK $cflags -dynamiclib $TMP/$name.$arch.o \
      -install_name @rpath/$name $largs $LINK_FLAGS -o $TMP/$name.$arch
    slices+=($TMP/$name.$arch)
  done
  lipo -create $slices -output $TMP/$name
}

build_one libsimcam.dylib engine/src/simcam.m $LIB_FLAGS $SAN ${=EXTRA} -- $FRAMEWORKS -Wl,-oso_prefix,$ROOT/
xcrun dsymutil --oso-prepend-path=$ROOT $TMP/libsimcam.dylib -o $TMP/libsimcam.dylib.dSYM 2>/dev/null || true
build_one libsimcamstub.dylib engine/stub/stub.c $STUB_FLAGS --
build_one libsimcamloader.dylib engine/loader/loader.c $STUB_FLAGS --
build_one libsimcamrunnerhook.dylib engine/runnerhook/runnerhook.m -fobjc-arc -O1 -Wall -- -framework Foundation

# a build dir holds a matching loader + engine pair; its name is the hash of both (the stub is frozen and lives on its own)
H=$(cat $TMP/libsimcam.dylib $TMP/libsimcamloader.dylib | shasum | cut -c1-12); mkdir -p build/$H
cp $TMP/libsimcam.dylib $TMP/libsimcamloader.dylib build/$H/; rm -rf build/$H/libsimcam.dylib.dSYM
[[ -d $TMP/libsimcam.dylib.dSYM ]] && cp -R $TMP/libsimcam.dylib.dSYM build/$H/
S=$(shasum $TMP/libsimcamstub.dylib | cut -c1-12); mkdir -p build/$S; cp $TMP/libsimcamstub.dylib build/$S/
K=$(shasum $TMP/libsimcamrunnerhook.dylib | cut -c1-12); mkdir -p build/$K; cp $TMP/libsimcamrunnerhook.dylib build/$K/

# copies the simulator loads from (cp keeps the bytes, so the ad-hoc linker signature stays valid)
SB=$SIMCAM_HOME/builds; mkdir -p $SB/$H $SB/$S $SB/$K
cp build/$H/libsimcam.dylib build/$H/libsimcamloader.dylib $SB/$H/; cp build/$S/libsimcamstub.dylib $SB/$S/
cp build/$K/libsimcamrunnerhook.dylib $SB/$K/
cmp -s build/$H/libsimcam.dylib $SB/$H/libsimcam.dylib &&
  cmp -s build/$H/libsimcamloader.dylib $SB/$H/libsimcamloader.dylib &&
  cmp -s build/$S/libsimcamstub.dylib $SB/$S/libsimcamstub.dylib &&
  cmp -s build/$K/libsimcamrunnerhook.dylib $SB/$K/libsimcamrunnerhook.dylib

echo $SB/$H/libsimcam.dylib > build/current_dylib.txt
echo $SB/$H/libsimcam.dylib > build/${VARIANT}_dylib.txt
echo $SB/$S/libsimcamstub.dylib > build/current_stub.txt
echo $SB/$H/libsimcamloader.dylib > build/current_loader.txt
echo $SB/$H/libsimcamloader.dylib > build/${VARIANT}_loader.txt
echo $SB/$K/libsimcamrunnerhook.dylib > build/current_hook.txt

for f in $SB/$H/libsimcam.dylib $SB/$H/libsimcamloader.dylib $SB/$S/libsimcamstub.dylib $SB/$K/libsimcamrunnerhook.dylib; do
  for arch in $ARCHS; do
    echo "$f [$arch]: $(codesign -dv --arch $arch $f 2>&1 | grep -E 'Signature|flags' | tr '\n' ' ')"
  done
done
echo "variant=$VARIANT"
echo "dylib=$(cat build/current_dylib.txt)"
echo "loader=$(cat build/current_loader.txt)"
echo "stub=$(cat build/current_stub.txt)"
echo "hook=$(cat build/current_hook.txt)"
