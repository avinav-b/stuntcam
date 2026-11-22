#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# engine/src/providers/qr_test.sh — host tests for providers/qr.c (the `qr:` source's spec decoding and code size) under
# ASan + UBSan. Runs on macOS and Linux (CC defaults to clang, else cc). Exit 0 only if every check passed.
set -eu
DIR="$(cd "$(dirname "$0")" && pwd -P)"
CC=${CC:-$(command -v clang || command -v cc)}
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT   # this script's own mktemp dir (clang -g on macOS also leaves qr_test.dSYM there)
"$CC" -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -o "$OUT/qr_test" "$DIR/qr_test.c"
"$OUT/qr_test"
