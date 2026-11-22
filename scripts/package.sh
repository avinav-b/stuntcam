#!/bin/zsh
# SPDX-License-Identifier: Apache-2.0
# scripts/package.sh [--verdicts DIR] [--no-build] [--allow-dirty] [--helper APP] — build the v1 packages locally.
# Publishes nothing.
#
#   build/package/stuntcam/            the npm package directory (installed layout, artifacts.Installed):
#     package.json  README.md  LICENSE  THIRD_PARTY_NOTICES.md
#     bin/stuntcam                                         the Go CLI, darwin/arm64, linker ad-hoc signed (no JS shim),
#                                                          built after share/ is staged, with the sha256 of manifest.txt
#                                                          and payload.tar embedded (-ldflags -X): the CLI refuses a
#                                                          share/ it wasn't packaged with
#     share/stuntcam/payload.tar                           every Mach-O file the simulator loads or runs, byte-identical
#                                                          (linker-signed), in an uncompressed tar that no package
#                                                          manager relinks or re-signs; the CLI checks and unpacks it
#                                                          into $SIMCAM_HOME/builds/<hash>/ and $SIMCAM_HOME/canary/:
#         builds/<pair>/libsimcam{,loader}.dylib           ./build.sh's products
#         stub/<sha>/libsimcamstub.dylib                   the frozen stub (FROZEN_STUB below; any other hash is refused)
#         hook/<sha>/libsimcamrunnerhook.dylib             the XCTest runner hook (`stuntcam exec`)
#         canary/m10canary.app                             the prebuilt canary
#         helper/<sha>/Stuntcam Camera.app                 the camera helper (`webcam` source), as signed: inside the
#                                                          tar, since Homebrew re-signs loose Mach-O files, which would
#                                                          break its signature and the camera grant that follows it
#     share/stuntcam/profiles/cfg-m10.plist  media/qr.png  the shipped profile and the scene's image
#     share/stuntcam/restore-rules/<xcode build>.json
#     share/stuntcam/verdicts/<key>.json                   shipped canary passes (only with --verdicts)
#     share/stuntcam/manifest.txt                          version, commit, build ids, packager Xcode, sha256 of every
#                                                          file but bin/stuntcam (which embeds the manifest's sha256)
#                                                          and of every payload member (checked at every use)
#   build/package/stuntcam-<version>.tgz   `npm pack` of it (also the Homebrew/release tarball)
#   build/package/filelist.txt             `npm pack --dry-run` file list
#   build/package/stuntcam.rb              the Homebrew formula, sha256 of that tarball, URL a TODO placeholder
#   build/package/setup-stuntcam/          the GitHub action (packaging/setup-stuntcam/action.template.yml, version filled in)
#
# The camera helper: --helper APP packages a release helper (bundle id dev.stuntcam.camera, Developer ID with the team
# $HELPER_TEAM_ID, hardened runtime, the camera entitlement only, notarization ticket stapled), checked before it enters
# the payload, and manifest line 2 says `helper release`. Without --helper it packages the dev helper from
# helper/build.sh (dev.stuntcam.camera.dev, ad-hoc or $STUNTCAM_DEV_SIGN; built first unless --no-build) and says
# `helper dev`: the release workflow's stage job refuses such a package. Line 2 also records the helper's <sha>, the
# sha256 of its executable and its CDHash, before the trailing `xcode <build>` field.
#
# The version is the VERSION file (cli/build.sh stamps the same one into the CLI with the commit). By default it runs
# ./build.sh first (normal variant) so the package is the tree's build; --no-build packages build/'s current output as is.
# --verdicts DIR copies the plain canary passes for exactly this build (engine, stub, canary, profile) from DIR (a
# $SIMCAM_HOME/verdicts, e.g. scripts/ci-verdicts.sh's runs of this build), without device ids or local paths;
# $VERDICTS_FROM names their source in each verdict's note (default "release-gate canary pass"). A dirty tree is
# refused (the CLI's commit would not name the sources) unless --allow-dirty. npm runs with npm_config_cache under build/package/.
set -euo pipefail
LROOT="$(cd "$(dirname "$0")/.." && pwd -L)"   # both forms are leak-checked
ROOT="$(cd "$LROOT" && pwd -P)"
cd "$ROOT"

# The stub is frozen: a change to it needs its own review and is never a side effect of packaging.
FROZEN_STUB=09908f5dbe40

VERDICTS=
BUILD=1
DIRTY=0
HELPER=
while (( $# )); do
  case $1 in
    --verdicts)
      VERDICTS=${2:?--verdicts DIR}
      shift
      ;;
    --no-build)
      BUILD=0
      ;;
    --allow-dirty)
      DIRTY=1
      ;;
    --helper)
      HELPER=${2:?--helper APP}
      shift
      ;;
    -h|--help)
      awk 'NR < 3 { next } !/^#/ { exit } { print }' "$0"   # the header comment
      exit 0
      ;;
    *)
      echo "package.sh: unknown argument $1" >&2
      exit 2
      ;;
  esac
  shift
done

die() { print -u2 "package.sh: $*"; exit 1 }

PKG_VERSION=$(<VERSION)
[[ $PKG_VERSION =~ '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' ]] || die "VERSION is not semver: $PKG_VERSION"

clean() { [[ -z $(git status --porcelain $@) ]] }
if ! clean && (( ! DIRTY )); then
  die "uncommitted changes (git status); commit first or --allow-dirty"
fi

command -v npm >/dev/null || die "npm not found"

OUT_DIR=$ROOT/build/package
PKG=$OUT_DIR/stuntcam
export npm_config_cache=$OUT_DIR/npm-cache npm_config_update_notifier=false
rm -rf $PKG $OUT_DIR/stuntcam-*.tgz(N) $OUT_DIR/filelist.txt $OUT_DIR/cli $OUT_DIR/payload $OUT_DIR/stuntcam.rb \
  $OUT_DIR/setup-stuntcam
mkdir -p $PKG/bin $PKG/share/stuntcam

XCODE_BUILD=$(xcodebuild -version | sed -n 's/^Build version //p')
[[ -n $XCODE_BUILD ]] || die "can't read the Xcode build (xcodebuild -version)"

# The dylibs: ./build.sh output, checked (hash dirs, frozen stub, universal, ad-hoc linker-signed on every slice)
if (( BUILD )); then
  ./build.sh > $OUT_DIR/build.log 2>&1 || { tail -20 $OUT_DIR/build.log; die "./build.sh failed"; }
fi

for f in current_dylib current_loader current_stub current_hook normal_dylib; do
  [[ -f build/$f.txt ]] || die "no build/$f.txt: run ./build.sh"
done

[[ $(<build/current_dylib.txt) == $(<build/normal_dylib.txt) ]] ||
  die "the last ./build.sh was a sanitizer variant: run ./build.sh (no flags)"

# ($SIMCAM_HOME paths: may contain spaces)
LOADER="$(<build/current_loader.txt)"
STUB="$(<build/current_stub.txt)"
HOOK="$(<build/current_hook.txt)"
ENGINE="$(<build/current_dylib.txt)"

ENGINE_HASH=${LOADER:h:t}
STUB_HASH=${STUB:h:t}
HOOK_HASH=${HOOK:h:t}
[[ ${ENGINE:h:t} == $ENGINE_HASH ]] || die "engine and loader pointers disagree"

sha12() { cat "$@" | shasum | cut -c1-12 }
[[ $(sha12 build/$ENGINE_HASH/libsimcam.dylib build/$ENGINE_HASH/libsimcamloader.dylib) == $ENGINE_HASH ]] ||
  die "build/$ENGINE_HASH doesn't hash to its name"
[[ $(sha12 build/$STUB_HASH/libsimcamstub.dylib) == $STUB_HASH ]] ||
  die "build/$STUB_HASH/libsimcamstub.dylib doesn't hash to its name"
[[ $(sha12 build/$HOOK_HASH/libsimcamrunnerhook.dylib) == $HOOK_HASH ]] ||
  die "build/$HOOK_HASH/libsimcamrunnerhook.dylib doesn't hash to its name"

[[ $STUB_HASH == $FROZEN_STUB ]] || die "stub hash $STUB_HASH is not the frozen $FROZEN_STUB"

for f in build/$ENGINE_HASH/libsimcam.dylib build/$ENGINE_HASH/libsimcamloader.dylib \
  build/$STUB_HASH/libsimcamstub.dylib build/$HOOK_HASH/libsimcamrunnerhook.dylib; do
  [[ $(lipo -archs $f) == "x86_64 arm64" ]] || die "$f is not universal x86_64 + arm64: $(lipo -archs $f)"
  for a in arm64 x86_64; do
    [[ $(codesign -dv --arch $a $f 2>&1) == *'flags=0x20002(adhoc,linker-signed)'* ]] ||
      die "$f [$a] is not ad-hoc linker-signed"
  done
done

# The canary, built from this tree (the CLI is built further down, once share/ is staged)
canary/build.sh > $OUT_DIR/canary.log 2>&1 || { cat $OUT_DIR/canary.log; die "canary/build.sh failed"; }
codesign --verify canary/build/m10canary.app || die "canary signature"

COMMIT=$(git rev-parse --short=12 HEAD)
clean --untracked-files=no || COMMIT=$COMMIT-dirty

# The camera helper: a release bundle (--helper) or the dev build, checked as it will be launched
if [[ -n $HELPER ]]; then
  HELPER_KIND=release
  HELPER_APP=${HELPER:A}
  HELPER_ID=dev.stuntcam.camera
else
  HELPER_KIND=dev
  HELPER_APP="$ROOT/build/helper/Stuntcam Camera.app"
  HELPER_ID=dev.stuntcam.camera.dev
  if (( BUILD )); then
    helper/build.sh > $OUT_DIR/helper.log 2>&1 || { tail -20 $OUT_DIR/helper.log; die "helper/build.sh failed"; }
  fi
fi

[[ -d $HELPER_APP && ${HELPER_APP:t} == "Stuntcam Camera.app" ]] ||
  die "no camera helper at $HELPER_APP (helper/build.sh, or --helper <Stuntcam Camera.app>)"

if ! codesign --verify --strict "$HELPER_APP" 2>$OUT_DIR/helper-verify.log; then
  cat $OUT_DIR/helper-verify.log
  die "the camera helper's signature doesn't verify (an unsigned --release build? sign it, or run helper/build.sh)"
fi

HELPER_SIGNING=$(codesign -dv --verbose=4 "$HELPER_APP" 2>&1)
[[ $HELPER_SIGNING == *"Identifier=$HELPER_ID"$'\n'* ]] || die "the camera helper's bundle id isn't $HELPER_ID"
# flags=0x10000(runtime) for Developer ID, 0x10002(adhoc,runtime) for the ad-hoc dev helper
HELPER_FLAGS=$(print -r -- $HELPER_SIGNING | sed -n 's/^CodeDirectory .*flags=0x[0-9a-f]*(\([^)]*\)).*/\1/p')
[[ ,$HELPER_FLAGS, == *,runtime,* ]] ||
  die "the camera helper isn't signed with the hardened runtime (flags: ${HELPER_FLAGS:-none found})"
# no entitlements (or none readable): empty, refused below
HELPER_ENTITLEMENTS=$(codesign -d --entitlements - --xml "$HELPER_APP" 2>/dev/null | plutil -convert json -o - - \
  2>/dev/null) || HELPER_ENTITLEMENTS=
[[ $HELPER_ENTITLEMENTS == '{"com.apple.security.device.camera":true}' ]] ||
  die "the camera helper's entitlements aren't exactly com.apple.security.device.camera: $HELPER_ENTITLEMENTS"

if [[ $HELPER_KIND == release ]]; then
  [[ -n ${HELPER_TEAM_ID:-} ]] || die "--helper needs HELPER_TEAM_ID (the Developer ID team it must be signed by)"
  [[ $HELPER_SIGNING == *"TeamIdentifier=$HELPER_TEAM_ID"$'\n'* ]] ||
    die "the camera helper isn't signed by team $HELPER_TEAM_ID"
  [[ $HELPER_SIGNING == *"Authority=Developer ID Application: "* ]] ||
    die "the camera helper isn't signed with a Developer ID Application certificate"
  if ! xcrun stapler validate "$HELPER_APP" > $OUT_DIR/helper-staple.log 2>&1; then
    cat $OUT_DIR/helper-staple.log
    die "the camera helper has no valid stapled notarization ticket"
  fi
fi

if find "$HELPER_APP" -type l | grep -q .; then
  die "the camera helper bundle contains a symlink (the payload tar is built from regular files only)"
fi

for f in "$HELPER_APP"/Contents/MacOS/*(N); do
  [[ -f $f && -x $f ]] || die "${f#$HELPER_APP/} isn't an executable regular file"
done

# the helper's <sha>: every file's relative path and sha1, sorted, hashed
HELPER_HASH=$(cd "$HELPER_APP" && LC_ALL=C find . -type f | LC_ALL=C sort |
  while IFS= read -r f; do print -r -- "${f#./} $(shasum < "$f" | cut -c1-40)"; done | shasum | cut -c1-12)

HELPER_EXE_SHA=$(shasum -a 256 "$HELPER_APP/Contents/MacOS/stuntcam-camera" | cut -d' ' -f1)
HELPER_CDHASH=$(print -r -- $HELPER_SIGNING | sed -n 's/^CDHash=//p')
[[ $HELPER_CDHASH =~ '^[0-9a-f]{40}$' ]] || die "can't read the camera helper's CDHash"

# Stage the installed layout. The Mach-O files go into payload.tar (cp/ditto keep the bytes, so the linker signatures
# stay valid): Homebrew's keg relinking rewrites install names and re-signs loose Mach-O files under the prefix, never
# inside an archive.
SHARE=$PKG/share/stuntcam
PAYLOAD=$OUT_DIR/payload
mkdir -p $PAYLOAD/builds/$ENGINE_HASH $PAYLOAD/stub/$STUB_HASH $PAYLOAD/hook/$HOOK_HASH $PAYLOAD/canary \
  $PAYLOAD/helper/$HELPER_HASH $SHARE/profiles $SHARE/media $SHARE/restore-rules

cp build/$ENGINE_HASH/libsimcam.dylib build/$ENGINE_HASH/libsimcamloader.dylib $PAYLOAD/builds/$ENGINE_HASH/
cp build/$STUB_HASH/libsimcamstub.dylib $PAYLOAD/stub/$STUB_HASH/
cp build/$HOOK_HASH/libsimcamrunnerhook.dylib $PAYLOAD/hook/$HOOK_HASH/
ditto --norsrc --noextattr canary/build/m10canary.app $PAYLOAD/canary/m10canary.app
ditto --norsrc --noextattr "$HELPER_APP" "$PAYLOAD/helper/$HELPER_HASH/Stuntcam Camera.app"

cp engine/profiles/cfg-m10.plist $SHARE/profiles/
cp fixtures/media/qr.png $SHARE/media/
cp restore-rules/[0-9]*.json $SHARE/restore-rules/
cp LICENSE THIRD_PARTY_NOTICES.md $PKG/
cp packaging/npm/README.md $PKG/README.md

(cd $PAYLOAD && find . -type f -exec chmod go-w {} + && find . -exec touch -h -t 198510260000 {} +)

# ustar, no xattrs/AppleDouble files, no owner names; members in sorted order (a stable tar for the same files)
(cd $PAYLOAD && find builds stub hook canary helper -type f | LC_ALL=C sort > $OUT_DIR/payload.list \
  && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --no-mac-metadata --no-acls \
     --uid 0 --gid 0 --uname root --gname wheel -cf $SHARE/payload.tar -T $OUT_DIR/payload.list)
diff <(tar -tf $SHARE/payload.tar) $OUT_DIR/payload.list ||
  die "payload.tar doesn't list exactly the staged Mach-O files"

CANARY_SHA=$(shasum canary/build/m10canary.app/m10canary | cut -c1-8)
PROFILE_SHA=$(sha12 engine/profiles/cfg-m10.plist)

N_VERDICTS=0
if [[ -n $VERDICTS ]]; then
  [[ -d $VERDICTS ]] || die "--verdicts $VERDICTS: not a directory"
  mkdir -p $SHARE/verdicts
  KEY_SUFFIX="-$ENGINE_HASH-$STUB_HASH-c$CANARY_SHA-$PROFILE_SHA"
  N_VERDICTS=$(node - "$VERDICTS" "$SHARE/verdicts" "$KEY_SUFFIX" "$PKG_VERSION" "$COMMIT" "$XCODE_BUILD" \
    "${VERDICTS_FROM:-release-gate canary pass}" <<'JS'
const fs = require('fs'), path = require('path');
const [src, dst, suffix, version, commit, xcode, from] = process.argv.slice(2);
let n = 0;
for (const f of fs.readdirSync(src).filter(f => f.endsWith('.json')).sort()) {
  let r;
  try {
    r = JSON.parse(fs.readFileSync(path.join(src, f), 'utf8'));
  } catch {
    continue;
  }
  // a plain pass for exactly this build: retried/relaunched/confirmed passes are short-lived by design, fails stay
  // local
  if (r.verdict !== 'pass' || !r.key || f !== r.key + '.json' || !r.key.endsWith(suffix) ||
      r.key !== r.runtime_build + suffix) continue;
  if (r.retried || r.relaunched || r.failed_first || /retried:\[/.test(r.result || '')) continue;
  const out = { proto: r.proto, key: r.key, verdict: r.verdict, result: r.result, runtime_build: r.runtime_build,
    runtime_version: r.runtime_version, engine_build: r.engine_build, stub_build: r.stub_build,
    canary_sha: r.canary_sha, profile_sha: r.profile_sha, device: '', at: r.at, packager_xcode: xcode,
    notes: [`shipped with stuntcam ${version} (commit ${commit}, packaged with Xcode ${xcode}): ${from}`] };
  fs.writeFileSync(path.join(dst, f), JSON.stringify(out, null, 1) + '\n');
  n++;
}
console.log(n);
JS
)
  (( N_VERDICTS > 0 )) || die "--verdicts $VERDICTS: no plain canary pass for this build (*$KEY_SUFFIX)"
fi

node - "$PKG/package.json" "$PKG_VERSION" <<'JS'
const [out, version] = process.argv.slice(2);
const pkg = {
  name: 'stuntcam', version,
  description: 'A camera for the iOS Simulator: your app\'s real AVFoundation camera code runs unmodified',
  keywords: ['ios', 'ios-simulator', 'simulator', 'camera', 'avfoundation', 'xcode', 'testing', 'xcuitest', 'ci',
    'react-native', 'visioncamera', 'expo', 'expo-camera', 'flutter', 'mobile_scanner', 'qr-code'],
  license: 'Apache-2.0',
  homepage: 'https://stuntcam.dev',
  repository: { type: 'git', url: 'git+https://github.com/avinav-b/stuntcam.git' },
  bugs: { url: 'https://github.com/avinav-b/stuntcam/issues' },
  bin: { stuntcam: 'bin/stuntcam' },
  os: ['darwin'], cpu: ['arm64'],
  files: ['bin/stuntcam', 'share/stuntcam/', 'THIRD_PARTY_NOTICES.md'],
  preferUnplugged: true,
};
require('fs').writeFileSync(out, JSON.stringify(pkg, null, 2) + '\n');
JS

# the CLI checks every share/stuntcam line and every payload/ line at each use; the other lines are informational
# (Homebrew installs only bin/ and share/). bin/stuntcam isn't listed: it is built next, with this manifest's sha256 in
# it (and may be re-signed by Homebrew, which doesn't change the embedded values).
[[ ! -e $PKG/bin/stuntcam ]] || die "bin/stuntcam exists before the manifest"

# manifest line 2: the build ids, the shipped verdicts, the helper, the packager's Xcode
BUILD_IDS="engine+loader $ENGINE_HASH stub $STUB_HASH hook $HOOK_HASH canary $CANARY_SHA profile $PROFILE_SHA"
BUILD_IDS+=" verdicts $N_VERDICTS helper $HELPER_KIND $HELPER_HASH sha256 $HELPER_EXE_SHA cdhash $HELPER_CDHASH"
BUILD_IDS+=" xcode $XCODE_BUILD"
{
  print "stuntcam $PKG_VERSION commit $COMMIT"
  print "$BUILD_IDS"
  (cd $PKG && find . -type f ! -path ./share/stuntcam/manifest.txt | LC_ALL=C sort | sed 's|^\./||' |
    while read -r f; do
      print -r -- "$(shasum -a 256 "$f" | cut -d' ' -f1)  $f"
    done)
  (cd $PAYLOAD && while read -r f; do
     print -r -- "$(shasum -a 256 "$f" | cut -d' ' -f1)  payload/$f"
   done < $OUT_DIR/payload.list)
} > $SHARE/manifest.txt

# nothing loose under share/ is Mach-O (the point of the payload)
for f in $SHARE/**/*(.); do
  [[ $(file -b "$f") != Mach-O* ]] || die "${f#$PKG/} is a loose Mach-O file: it belongs in payload.tar"
done

# The CLI, built now that share/ is final: the sha256 of manifest.txt and payload.tar go into the binary (-X), so it
# refuses a share/stuntcam it wasn't packaged with (integrity, and authenticity relative to the binary)
MANIFEST_SHA=$(shasum -a 256 $SHARE/manifest.txt | cut -d' ' -f1)
PAYLOAD_SHA=$(shasum -a 256 $SHARE/payload.tar | cut -d' ' -f1)
ARTIFACTS_PKG=github.com/avinav-b/stuntcam/cli/internal/artifacts
LDFLAGS_X="-X $ARTIFACTS_PKG.packagedManifest=$MANIFEST_SHA -X $ARTIFACTS_PKG.packagedPayload=$PAYLOAD_SHA" \
  GOARCHS=arm64 OUT=$OUT_DIR/cli cli/build.sh > $OUT_DIR/cli.log 2>&1 \
  || { cat $OUT_DIR/cli.log; die "cli/build.sh failed"; }

[[ $($OUT_DIR/cli/stuntcam version) == "stuntcam $PKG_VERSION (commit $COMMIT)" ]] ||
  die "CLI says '$($OUT_DIR/cli/stuntcam version)', want 'stuntcam $PKG_VERSION (commit $COMMIT)'"

# no grep -q under pipefail: SIGPIPE
[[ $(codesign -dv $OUT_DIR/cli/stuntcam 2>&1) == *adhoc* ]] || die "CLI binary is not ad-hoc signed"
cp $OUT_DIR/cli/stuntcam $PKG/bin/stuntcam && chmod 755 $PKG/bin/stuntcam

# the staged package checks itself with the embedded hashes, from a scratch host dir (and from outside any checkout)
CHECK_HOME=$OUT_DIR/check-home
rm -rf $CHECK_HOME
if ! out=$(cd / && SIMCAM_HOME=$CHECK_HOME $PKG/bin/stuntcam version --check 2>&1); then
  print -r -- "$out"
  die "version --check of the staged package failed"
fi
if [[ $out != *"packaged with (manifest sha256 ${MANIFEST_SHA[1,12]})"* ]]; then
  print -r -- "$out"
  die "the staged CLI doesn't carry the manifest's sha256"
fi

# the helper the CLI unpacked from the tar still verifies (exec bits and every sealed file survived the round trip)
codesign --verify --strict "$CHECK_HOME/helper/$HELPER_HASH/Stuntcam Camera.app" ||
  die "the camera helper unpacked from payload.tar doesn't verify"
rm -rf $CHECK_HOME

# No build-machine paths, no secrets, nothing unexpected
# fixed strings (-F: a path with regex characters still matches itself); grep exits 0 = found, 1 = none, 2 = error (a
# failure too, never "clean"). payload.tar is uncompressed, so its members are scanned as well (and unpacked, below).
scan() {  # scan <what> <grep args…>: die if any file in $PKG (or the unpacked payload) matches
  local what=$1 out rc
  shift
  out=$(LC_ALL=C grep -rla "$@" $PKG $PAYLOAD) && rc=0 || rc=$?
  if (( rc == 0 )); then
    die "$what in: $out"
  fi
  if (( rc != 1 )); then
    die "grep failed (exit $rc) scanning for $what"
  fi
}

scan "build-machine paths" -F -e "$ROOT" -e "$LROOT" -e "$HOME" -e /Users/ -e /private/var/folders \
  -e "$(hostname -s).local" -e stuntcam-work
scan "secret-looking strings" -E \
  -e 'BEGIN [A-Z ]*PRIVATE KEY|gh[pousr]_[A-Za-z0-9]{30,}|npm_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}|_authToken'

# npm pack (+ the dry-run file list), then check the tarball against the staged tree
(cd $OUT_DIR && npm pack ./stuntcam --dry-run 2>&1 | sed -n '/Tarball Contents/,/Tarball Details/p' > filelist.txt)
if ! (cd $OUT_DIR && npm pack ./stuntcam --pack-destination . > pack.log 2>&1); then
  cat $OUT_DIR/pack.log
  die "npm pack failed"
fi

TGZ=$OUT_DIR/stuntcam-$PKG_VERSION.tgz
[[ -f $TGZ ]] || die "no $TGZ"
diff <(tar -tzf $TGZ | sed 's|^package/||' | grep -v '/$' | LC_ALL=C sort) \
  <(cd $PKG && find . -type f | sed 's|^\./||' | LC_ALL=C sort) \
  || die "tarball and staged tree differ"

[[ $(tar -tvzf $TGZ package/bin/stuntcam) == -rwxr-xr-x* ]] || die "bin/stuntcam lost its exec bit in the tarball"
[[ $(tar -tvf $SHARE/payload.tar canary/m10canary.app/m10canary) == -rwxr-xr-x* ]] ||
  die "the canary lost its exec bit in payload.tar"
for f in "$PAYLOAD/helper/$HELPER_HASH/Stuntcam Camera.app"/Contents/MacOS/*(N); do
  [[ $(tar -tvf $SHARE/payload.tar "${f#$PAYLOAD/}") == -rwxr-xr-x* ]] ||
    die "${f#$PAYLOAD/} lost its exec bit in payload.tar"
done

TGZ_SHA=$(shasum -a 256 $TGZ | cut -d' ' -f1)

# The Homebrew formula and the GitHub action (generated here, never committed; not published; the URL is a
# placeholder). The formula's text is output: its long comment lines stay as they are.
cat > $OUT_DIR/stuntcam.rb <<RB
# SPDX-License-Identifier: Apache-2.0
# Generated by scripts/package.sh from stuntcam-$PKG_VERSION.tgz (commit $COMMIT). NOT PUBLISHED: no tap exists yet.
# The tarball is the npm package; Homebrew strips its top "package/" dir. bin/stuntcam finds share/stuntcam next to it
# (cli/internal/artifacts: the installed layout). The simulator's Mach-O files ship inside share/stuntcam/payload.tar,
# so Homebrew's keg relinking (install names, re-signing) never touches them; the CLI checks every file against
# share/stuntcam/manifest.txt and unpacks the payload into \$SIMCAM_HOME at each use.
class Stuntcam < Formula
  desc "Camera for the iOS Simulator: real AVFoundation, no app changes"
  homepage "https://stuntcam.dev"
  # TODO(release): placeholder URL. Upload stuntcam-$PKG_VERSION.tgz to the v$PKG_VERSION release (or point at the npm registry tarball,
  # https://registry.npmjs.org/stuntcam/-/stuntcam-$PKG_VERSION.tgz, once published) and keep sha256 in sync with that file.
  url "https://github.com/avinav-b/stuntcam/releases/download/v$PKG_VERSION/stuntcam-$PKG_VERSION.tgz"
  sha256 "$TGZ_SHA"
  license "Apache-2.0"

  depends_on arch: :arm64
  depends_on :macos

  def install
    bin.install "bin/stuntcam"
    share.install "share/stuntcam"
  end

  def caveats
    <<~EOS
      Needs Xcode 27 with iOS Simulator runtimes. Then: stuntcam up
    EOS
  end

  test do
    assert_match "stuntcam #{version}", shell_output("#{bin}/stuntcam version")
    # nothing loose under share/ is Mach-O: the dylibs and the canary are inside payload.tar
    Dir[share/"stuntcam/**/*"].select { |f| File.file?(f) }.each do |f|
      refute_match(/Mach-O/, shell_output("file -b #{f}"), "#{f} is a loose Mach-O file")
    end
    # every file matches the manifest, and the payload unpacks into a private host directory
    ENV["SIMCAM_HOME"] = (testpath/"home").to_s
    assert_match "every file matches", shell_output("#{bin}/stuntcam version --check")
    # the camera helper is unpacked from the payload with its signature intact
    helpers = Dir[testpath/"home/helper/*/Stuntcam Camera.app"]
    assert_equal 1, helpers.length
    system "codesign", "--verify", "--strict", helpers.first
    dylibs = Dir[testpath/"home/builds/*/*.dylib"]
    assert_equal 4, dylibs.length
    # the unpacked dylibs are the packaged bytes: ad-hoc linker-signed on every slice
    dylibs.each do |f|
      %w[arm64 x86_64].each do |arch|
        assert_match "flags=0x20002(adhoc,linker-signed)", shell_output("codesign -dv --arch #{arch} #{f} 2>&1")
      end
    end
  end
end
RB

mkdir -p $OUT_DIR/setup-stuntcam
sed "s/@VERSION@/$PKG_VERSION/g" packaging/setup-stuntcam/action.template.yml |
  sed '/^# setup-stuntcam (design/,/^# a public repo/c\
# setup-stuntcam (design §10, CI), generated by scripts/package.sh for stuntcam '"$PKG_VERSION"' (commit '"$COMMIT"'). NOT PUBLISHED.
' > $OUT_DIR/setup-stuntcam/action.yml
cp packaging/setup-stuntcam/README.md $OUT_DIR/setup-stuntcam/
[[ $(<$OUT_DIR/setup-stuntcam/action.yml) != *@VERSION@* ]] || die "action.yml: @VERSION@ left"

print "version $PKG_VERSION commit $COMMIT"
print "manifest sha256 $MANIFEST_SHA  payload sha256 $PAYLOAD_SHA (embedded in bin/stuntcam)"
print "engine+loader $ENGINE_HASH  stub $STUB_HASH (frozen)  hook $HOOK_HASH" \
  " canary $CANARY_SHA  profile $PROFILE_SHA  shipped verdicts $N_VERDICTS"
print "camera helper $HELPER_HASH ($HELPER_KIND, $HELPER_ID, CDHash $HELPER_CDHASH)"
print "package dir: $PKG"
print "tarball: $TGZ ($(stat -f %z $TGZ) bytes, unpacked $(du -sk $PKG | cut -f1) KiB," \
  "$(tar -tzf $TGZ | grep -vc '/$') files)"
print "sha256 $TGZ_SHA"
print "formula: $OUT_DIR/stuntcam.rb"
print "action: $OUT_DIR/setup-stuntcam/action.yml"
