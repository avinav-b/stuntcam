#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# cli/build.sh [VERSION] — build the stuntcam CLI for macOS (arm64 + x86_64; universal with lipo when run on a Mac).
#   Pure Go (CGO off), so it cross-compiles from Linux too. Output: cli/bin/stuntcam (universal or per-arch files).
#   VERSION defaults to the repo's VERSION file (the one version source: scripts/package.sh writes it into
#   package.json and the generated Homebrew formula). The commit is `git rev-parse --short=12 HEAD`, plus "-dirty" for
#   uncommitted changes. `stuntcam version` prints both.
#   GOARCHS="arm64" builds one arch (scripts/package.sh). OUT=<dir> writes there, not to bin/.
#   LDFLAGS_X="-X pkg.var=value …" adds link-time values (scripts/package.sh passes the sha256 of the package's
#   manifest.txt and payload.tar, which anchor the installed files to this binary; a build without them is a dev build).
set -eu
cd "$(dirname "$0")"

CLI_VERSION=${1:-$(cat ../VERSION 2>/dev/null || echo 0.0.0-dev)}
CLI_COMMIT=$(git rev-parse --short=12 HEAD 2>/dev/null || echo "")
if [ -n "$CLI_COMMIT" ] && [ -n "$(git status --porcelain --untracked-files=no -- .. 2>/dev/null)" ]; then
  CLI_COMMIT="$CLI_COMMIT-dirty"
fi

GO=${GO:-go}
GOARCHS=${GOARCHS:-arm64 amd64}
OUT=${OUT:-bin}

link_flags="-s -w -X main.version=$CLI_VERSION -X main.commit=$CLI_COMMIT ${LDFLAGS_X:-}"
mkdir -p "$OUT"
arch_count=0
for arch in $GOARCHS; do
  GOOS=darwin GOARCH=$arch CGO_ENABLED=0 $GO build -trimpath -ldflags "$link_flags" -o "$OUT/stuntcam-$arch" \
    ./cmd/stuntcam
  arch_count=$((arch_count + 1))
  last_arch=$arch
done

if [ $arch_count -eq 1 ]; then
  mv "$OUT/stuntcam-$last_arch" "$OUT/stuntcam"   # the Go linker ad-hoc signs a darwin/arm64 binary itself
  echo "$OUT/stuntcam ($CLI_VERSION, commit $CLI_COMMIT, $last_arch)"
elif command -v lipo >/dev/null 2>&1; then
  set --
  for arch in $GOARCHS; do
    set -- "$@" "$OUT/stuntcam-$arch"
  done
  lipo -create "$@" -output "$OUT/stuntcam" && rm "$@"
  # lipo drops the per-slice signature check, so the fat file is signed again, ad hoc.
  codesign -s - -f "$OUT/stuntcam" >/dev/null 2>&1 || true
  echo "$OUT/stuntcam ($CLI_VERSION, commit $CLI_COMMIT, universal)"
else
  echo "$OUT/stuntcam-* ($CLI_VERSION, commit $CLI_COMMIT; run on a Mac for a universal binary)"
fi
