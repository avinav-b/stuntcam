#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# spdx-check.sh — list tracked source files that don't start with an SPDX-License-Identifier line
#   Checks git-tracked files under build.sh, engine/, cli/, canary/, scripts/, tools/ and fixtures/ (not docs/).
#   A source file is one with a code extension (.c .h .m .inc .swift .go .py .sh .ts .tsx .js .dart .yml .yaml), go.mod,
#   or any file whose first line is a shebang. The SPDX line must be line 1, or line 2 after a shebang.
#   Data and generated files (plists, JSON, Xcode project files, images, lockfiles, Markdown) are out of scope.
#   Third-party and template-generated files are excluded below; THIRD_PARTY_NOTICES.md lists them instead.
#   Exit 0 if none is missing, 1 if any is (each printed on stdout).
set -eu
cd "$(dirname "$0")/.."

# Third-party / template files: not ours, so they don't claim Apache-2.0 (THIRD_PARTY_NOTICES.md)
EXCLUDE='
fixtures/expo/index.ts
fixtures/expo/ios/m1bcam/AppDelegate.swift
fixtures/expo/ios/m1bcam/SceneDelegate.swift
fixtures/expo/ios/m1bcam/m1bcam-Bridging-Header.h
fixtures/flutter/analysis_options.yaml
fixtures/flutter/pubspec.yaml
fixtures/flutter/ios/Runner/AppDelegate.swift
fixtures/flutter/ios/Runner/SceneDelegate.swift
fixtures/flutter/ios/Runner/Runner-Bridging-Header.h
fixtures/flutter/ios/RunnerTests/RunnerTests.swift
'

missing=0
list=$(mktemp "${TMPDIR:-/tmp}/spdx-check.XXXXXX") || exit 2
trap 'rm -f "$list"' EXIT
git -c core.quotePath=false ls-files -- build.sh engine cli canary scripts tools fixtures > "$list" || exit 2
while IFS= read -r f; do
  [ -f "$f" ] || continue
  # an excluded path: a whole line of EXCLUDE
  case "$EXCLUDE" in
    *"
$f
"*)
      continue
      ;;
  esac

  src=0
  case "$f" in
    *.c|*.h|*.m|*.inc|*.swift|*.go|*.py|*.sh|*.ts|*.tsx|*.js|*.dart|*.yml|*.yaml|*/go.mod|go.mod)
      src=1
      ;;
    *.png|*.jpg|*.jpeg|*.mov|*.mp4|*.heic|*.plist|*.json|*.pbxproj|*.xcscheme|*.md|*.txt|*.ttf)
      continue
      ;;
  esac

  first=$(head -n 1 "$f" | tr -d '\r')
  case "$first" in
    '#!'*)
      src=1
      ;;
  esac

  [ "$src" = 1 ] || continue

  # The SPDX line: line 2 after a shebang, else line 1.
  case "$first" in
    '#!'*)
      line=$(sed -n 2p "$f")
      ;;
    *)
      line=$first
      ;;
  esac

  case "$line" in
    *SPDX-License-Identifier:\ Apache-2.0*)
      ;;
    *)
      echo "$f"
      missing=$((missing + 1))
      ;;
  esac
done < "$list"

if [ "$missing" -gt 0 ]; then
  echo "spdx-check: $missing file(s) without an SPDX-License-Identifier header" >&2
  exit 1
fi

echo "spdx-check: ok" >&2
