#!/usr/bin/env zsh
# SPDX-License-Identifier: Apache-2.0
#
# engine/src/webcam/test.sh [--fuzz SECONDS] [--no-shm] — host tests for the webcam ring (docs/webcam-ring.md).
#
#   1. macOS only: ring.c and ring_shm.c compile as the engine would (Objective-C, iOS Simulator, -Werror), and
#      engine/src/simcam.m, which includes them in its single translation unit, compiles with the engine's flags and no
#      diagnostic from the webcam files (objects discarded).
#   2. ring_test.c under ASan + UBSan: validation edge cases, seqlock reads, the private writer against a rewritten
#      header, writer/reader and writer/scribbler stress runs, and the shm create/check/retire/re-open tests (it prints
#      what this OS does with fchmod, umask and taken names).
#   3. ring_fuzz.c for SECONDS (default 60): libFuzzer + ASan + UBSan over the reader, writer, pid-file and name
#      targets. With no `clang -fsanitize=fuzzer` (Apple's clang ships no libFuzzer runtime), this links the
#      sanitizers with a libFuzzer archive from $LIBFUZZER or a Homebrew LLVM; with neither, a deterministic mutation
#      loop under ASan + UBSan. The corpus and any crash inputs stay in build/webcam-fuzz/ (not tracked).
# Runs on macOS and Linux (steps 2 and 3; CC defaults to clang, else cc). Exit 0 only if every step passed and the
# fuzzer finished its run with nothing found.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd -P)"
cd "$ROOT"

FUZZ=60
SHM=()
while (( $# )); do
  case $1 in
    --fuzz)
      FUZZ=$2
      shift
      ;;
    --no-shm)
      SHM=(--no-shm)
      ;;
    -h|--help)
      sed -n '3,17p' "$0"   # the header comment
      exit 0
      ;;
    *)
      echo "test.sh: unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

WEBCAM_SRC=engine/src/webcam
OUT=build/webcam-fuzz
mkdir -p $OUT/corpus $OUT/crashes

if [[ $(uname) == Darwin ]]; then
  CC=(xcrun clang)
  FEAT=(-D_DARWIN_C_SOURCE)
  LIBS=()
else
  CC=(${CC:-$(command -v clang || command -v cc)})
  FEAT=(-D_GNU_SOURCE)
  LIBS=(-pthread -lrt)
fi

SAN=(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
WARN=(-Wall -Wextra -Wno-unused-parameter -Wno-unused-function)

if [[ $(uname) == Darwin ]]; then
  echo "== engine-style compile (objective-c, iOS Simulator), alone and inside simcam.m's translation unit"
  if ! grep -q '#include "webcam/ring.c"' engine/src/simcam.m; then
    echo "simcam.m doesn't include webcam/ring.c"
    exit 1
  fi

  SDK=$(xcrun --sdk iphonesimulator --show-sdk-path)
  SIM=(-target arm64-apple-ios17.0-simulator -isysroot $SDK -fobjc-arc -O1)
  for f in ring.c ring_shm.c; do
    $CC $SIM -x objective-c -Wall -Werror -c $WEBCAM_SRC/$f -o $OUT/${f%.c}.o
  done

  # The engine's own flags (build.sh). Any diagnostic from the webcam files fails the step.
  if ! $CC $SIM -Wall -Wno-unused-function -Wno-unused-variable -c engine/src/simcam.m -o $OUT/tu.o 2> $OUT/tu.log
  then
    cat $OUT/tu.log
    exit 1
  fi

  if grep -q "$WEBCAM_SRC/" $OUT/tu.log; then
    grep "$WEBCAM_SRC/" $OUT/tu.log
    exit 1
  fi
  echo "ok"
fi

echo "== unit tests (ASan + UBSan)"
$CC -std=c11 $FEAT -g -O1 $WARN $SAN -o $OUT/ring_test $WEBCAM_SRC/ring_test.c $LIBS
$OUT/ring_test $SHM

echo "== fuzz (${FUZZ}s)"
$CC -std=c11 $FEAT -DSCW_FUZZ_SEEDS -O1 $WARN -o $OUT/ring_seeds $WEBCAM_SRC/ring_fuzz.c $LIBS
$OUT/ring_seeds $OUT/corpus

ENGINE=""
if $CC -std=c11 $FEAT -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -g -O1 $WARN \
    -o $OUT/ring_fuzz $WEBCAM_SRC/ring_fuzz.c $LIBS 2>/dev/null; then
  ENGINE="libFuzzer (toolchain)"
else
  LIB=${LIBFUZZER:-}
  if [[ -z $LIB ]]; then
    for candidate in /opt/homebrew/opt/llvm/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a(N) \
      /usr/local/opt/llvm/lib/clang/*/lib/darwin/libclang_rt.fuzzer_osx.a(N); do
      LIB=$candidate   # the last match in glob (lexical) order
    done
  fi
  if [[ -n $LIB ]] && $CC -std=c11 $FEAT -fsanitize=fuzzer-no-link -g -O1 $WARN $SAN $WEBCAM_SRC/ring_fuzz.c $LIB \
      -lc++ $LIBS -o $OUT/ring_fuzz 2>/dev/null; then
    ENGINE="libFuzzer ($LIB) with the toolchain's ASan + UBSan"
  fi
fi

if [[ -n $ENGINE ]]; then
  echo "engine: $ENGINE"

  MAX_INPUT_BYTES=8192   # covers a full header plus a mutation script
  INPUT_TIMEOUT_S=10
  RSS_LIMIT_MB=4096      # stays above the 8 MiB mappings

  rc=0
  $OUT/ring_fuzz -max_total_time=$FUZZ -max_len=$MAX_INPUT_BYTES -timeout=$INPUT_TIMEOUT_S -rss_limit_mb=$RSS_LIMIT_MB \
    -print_final_stats=1 -artifact_prefix=$OUT/crashes/ $OUT/corpus > $OUT/fuzz.log 2>&1 || rc=$?
  grep -E '^(Done [0-9]+ runs|stat::|==[0-9]+==ERROR|SUMMARY)|runtime error' $OUT/fuzz.log || true

  crashes=($OUT/crashes/*(N))

  if (( rc != 0 )); then
    echo "FUZZ: the fuzzer exited with status $rc (log: $OUT/fuzz.log)"
    exit 1
  fi
  if (( ${#crashes} )); then
    echo "FUZZ: ${#crashes} crash input(s) in $OUT/crashes"
    exit 1
  fi
  if ! grep -qE '^Done [0-9]+ runs in' $OUT/fuzz.log; then
    echo "FUZZ: no 'Done N runs' line (log: $OUT/fuzz.log)"
    exit 1
  fi
else
  echo "engine: deterministic mutation loop (no libFuzzer runtime found)"
  $CC -std=c11 $FEAT -DSCW_FUZZ_STANDALONE -g -O1 $WARN $SAN -o $OUT/ring_fuzz_sa $WEBCAM_SRC/ring_fuzz.c $LIBS
  $OUT/ring_fuzz_sa -seconds $FUZZ
fi

echo "PASS"
