#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# cli/build.sh [VERSION] — build the stuntcam CLI for macOS (arm64 + x86_64; universal with lipo when run on a Mac).
#   Pure Go (CGO off), so it cross-compiles from Linux too. Output: cli/bin/stuntcam (universal or per-arch files).
#   VERSION defaults to the repo's VERSION file (the one version source: scripts/package.sh writes it into package.json
#   and the generated Homebrew formula); the commit is `git rev-parse --short=12 HEAD`, plus "-dirty" for uncommitted changes.
#   `stuntcam version` prints both. GOARCHS="arm64" builds one arch (scripts/package.sh); OUT=<dir> writes there, not bin/.
#   LDFLAGS_X="-X pkg.var=value …" adds link-time values (scripts/package.sh: the sha256 of the package's manifest.txt and
#   payload.tar, which anchor the installed files to this binary; a build without them is a dev build).
set -eu
cd "$(dirname "$0")"
V=${1:-$(cat ../VERSION 2>/dev/null || echo 0.0.0-dev)}
C=$(git rev-parse --short=12 HEAD 2>/dev/null || echo "")
if [ -n "$C" ] && [ -n "$(git status --porcelain --untracked-files=no -- .. 2>/dev/null)" ]; then C="$C-dirty"; fi
GO=${GO:-go}
GOARCHS=${GOARCHS:-arm64 amd64}
OUT=${OUT:-bin}
mkdir -p "$OUT"
n=0
for a in $GOARCHS; do
  GOOS=darwin GOARCH=$a CGO_ENABLED=0 $GO build -trimpath -ldflags "-s -w -X main.version=$V -X main.commit=$C ${LDFLAGS_X:-}" -o "$OUT/stuntcam-$a" ./cmd/stuntcam
  n=$((n + 1)); last=$a
done
if [ $n -eq 1 ]; then
  mv "$OUT/stuntcam-$last" "$OUT/stuntcam"   # the Go linker ad-hoc signs a darwin/arm64 binary itself
  echo "$OUT/stuntcam ($V, commit $C, $last)"
elif command -v lipo >/dev/null 2>&1; then
  set --; for a in $GOARCHS; do set -- "$@" "$OUT/stuntcam-$a"; done
  lipo -create "$@" -output "$OUT/stuntcam" && rm "$@"
  codesign -s - -f "$OUT/stuntcam" >/dev/null 2>&1 || true   # lipo drops the per-slice signature check; re-sign the fat file ad hoc
  echo "$OUT/stuntcam ($V, commit $C, universal)"
else
  echo "$OUT/stuntcam-* ($V, commit $C; run on a Mac for a universal binary)"
fi
