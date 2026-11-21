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
# $SIMCAM_HOME/verdicts of release-gate runs), without device ids or local paths. A dirty tree is refused (the CLI's
# commit would not name the sources) unless --allow-dirty. npm runs with npm_config_cache under build/package/.
set -euo pipefail
LROOT="$(cd "$(dirname "$0")/.." && pwd -L)"; ROOT="$(cd "$LROOT" && pwd -P)"; cd "$ROOT"   # both forms are leak-checked
FROZEN_STUB=09908f5dbe40   # the stub is frozen (design §4.1): a change needs its own review, never a package side effect

VERDICTS=; BUILD=1; DIRTY=0; HELPER=
while (( $# )); do
  case $1 in
    --verdicts) VERDICTS=${2:?--verdicts DIR}; shift;;
    --no-build) BUILD=0;;
    --allow-dirty) DIRTY=1;;
    --helper) HELPER=${2:?--helper APP}; shift;;
    -h|--help) awk 'NR < 3 { next } !/^#/ { exit } { print }' "$0"; exit 0;;
    *) echo "package.sh: unknown argument $1" >&2; exit 2;;
  esac
  shift
done
die() { print -u2 "package.sh: $*"; exit 1 }

V=$(<VERSION); [[ $V =~ '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$' ]] || die "VERSION is not semver: $V"
clean() { [[ -z $(git status --porcelain $@) ]] }
if ! clean && (( ! DIRTY )); then die "uncommitted changes (git status); commit first or --allow-dirty"; fi
command -v npm >/dev/null || die "npm not found"
OUTD=$ROOT/build/package; P=$OUTD/stuntcam; export npm_config_cache=$OUTD/npm-cache npm_config_update_notifier=false
rm -rf $P $OUTD/stuntcam-*.tgz(N) $OUTD/filelist.txt $OUTD/cli $OUTD/payload $OUTD/stuntcam.rb $OUTD/setup-stuntcam
mkdir -p $P/bin $P/share/stuntcam
XB=$(xcodebuild -version | sed -n 's/^Build version //p'); [[ -n $XB ]] || die "can't read the Xcode build (xcodebuild -version)"

# 1. the dylibs: ./build.sh output, checked (hash dirs, frozen stub, universal, ad-hoc linker-signed on every slice)
(( BUILD )) && { ./build.sh > $OUTD/build.log 2>&1 || { tail -20 $OUTD/build.log; die "./build.sh failed"; } }
for f in current_dylib current_loader current_stub current_hook normal_dylib; do [[ -f build/$f.txt ]] || die "no build/$f.txt: run ./build.sh"; done
[[ $(<build/current_dylib.txt) == $(<build/normal_dylib.txt) ]] || die "the last ./build.sh was a sanitizer variant: run ./build.sh (no flags)"
L="$(<build/current_loader.txt)"; ST="$(<build/current_stub.txt)"; HK="$(<build/current_hook.txt)"; EN="$(<build/current_dylib.txt)"
H=${L:h:t}; S=${ST:h:t}; K=${HK:h:t}   # ($SIMCAM_HOME paths: may contain spaces)
[[ ${EN:h:t} == $H ]] || die "engine and loader pointers disagree"
sha12() { cat "$@" | shasum | cut -c1-12 }
[[ $(sha12 build/$H/libsimcam.dylib build/$H/libsimcamloader.dylib) == $H ]] || die "build/$H doesn't hash to its name"
[[ $(sha12 build/$S/libsimcamstub.dylib) == $S ]] || die "build/$S/libsimcamstub.dylib doesn't hash to its name"
[[ $(sha12 build/$K/libsimcamrunnerhook.dylib) == $K ]] || die "build/$K/libsimcamrunnerhook.dylib doesn't hash to its name"
[[ $S == $FROZEN_STUB ]] || die "stub hash $S is not the frozen $FROZEN_STUB"
for f in build/$H/libsimcam.dylib build/$H/libsimcamloader.dylib build/$S/libsimcamstub.dylib build/$K/libsimcamrunnerhook.dylib; do
  [[ $(lipo -archs $f) == "x86_64 arm64" ]] || die "$f is not universal x86_64 + arm64: $(lipo -archs $f)"
  for a in arm64 x86_64; do
    [[ $(codesign -dv --arch $a $f 2>&1) == *'flags=0x20002(adhoc,linker-signed)'* ]] || die "$f [$a] is not ad-hoc linker-signed"
  done
done

# 2. the canary, built from this tree (the CLI is built in step 3b, once share/ is staged)
canary/build.sh > $OUTD/canary.log 2>&1 || { cat $OUTD/canary.log; die "canary/build.sh failed"; }
codesign --verify canary/build/m10canary.app || die "canary signature"
C=$(git rev-parse --short=12 HEAD); clean --untracked-files=no || C=$C-dirty

# 2b. the camera helper: a release bundle (--helper) or the dev build, checked as it will be launched
if [[ -n $HELPER ]]; then
  HKIND=release; HAPP=${HELPER:A}; HID=dev.stuntcam.camera
else
  HKIND=dev; HAPP="$ROOT/build/helper/Stuntcam Camera.app"; HID=dev.stuntcam.camera.dev
  (( BUILD )) && { helper/build.sh > $OUTD/helper.log 2>&1 || { tail -20 $OUTD/helper.log; die "helper/build.sh failed"; } }
fi
[[ -d $HAPP && ${HAPP:t} == "Stuntcam Camera.app" ]] || die "no camera helper at $HAPP (helper/build.sh, or --helper <Stuntcam Camera.app>)"
codesign --verify --strict "$HAPP" 2>$OUTD/helper-verify.log || { cat $OUTD/helper-verify.log; die "the camera helper's signature doesn't verify (an unsigned --release build? sign it, or run helper/build.sh)"; }
HCS=$(codesign -dv --verbose=4 "$HAPP" 2>&1)
[[ $HCS == *"Identifier=$HID"$'\n'* ]] || die "the camera helper's bundle id isn't $HID"
# flags=0x10000(runtime) for Developer ID, 0x10002(adhoc,runtime) for the ad-hoc dev helper
HFL=$(print -r -- $HCS | sed -n 's/^CodeDirectory .*flags=0x[0-9a-f]*(\([^)]*\)).*/\1/p')
[[ ,$HFL, == *,runtime,* ]] || die "the camera helper isn't signed with the hardened runtime (flags: ${HFL:-none found})"
HENT=$(codesign -d --entitlements - --xml "$HAPP" 2>/dev/null | plutil -convert json -o - - 2>/dev/null) || HENT=
[[ $HENT == '{"com.apple.security.device.camera":true}' ]] || die "the camera helper's entitlements aren't exactly com.apple.security.device.camera: $HENT"
if [[ $HKIND == release ]]; then
  [[ -n ${HELPER_TEAM_ID:-} ]] || die "--helper needs HELPER_TEAM_ID (the Developer ID team it must be signed by)"
  [[ $HCS == *"TeamIdentifier=$HELPER_TEAM_ID"$'\n'* ]] || die "the camera helper isn't signed by team $HELPER_TEAM_ID"
  [[ $HCS == *"Authority=Developer ID Application: "* ]] || die "the camera helper isn't signed with a Developer ID Application certificate"
  xcrun stapler validate "$HAPP" > $OUTD/helper-staple.log 2>&1 || { cat $OUTD/helper-staple.log; die "the camera helper has no valid stapled notarization ticket"; }
fi
if find "$HAPP" -type l | grep -q .; then die "the camera helper bundle contains a symlink (the payload tar is built from regular files only)"; fi
for f in "$HAPP"/Contents/MacOS/*(N); do [[ -f $f && -x $f ]] || die "${f#$HAPP/} isn't an executable regular file"; done
HS=$(cd "$HAPP" && LC_ALL=C find . -type f | LC_ALL=C sort | while IFS= read -r f; do print -r -- "${f#./} $(shasum < "$f" | cut -c1-40)"; done | shasum | cut -c1-12)
HEXE=$(shasum -a 256 "$HAPP/Contents/MacOS/stuntcam-camera" | cut -d' ' -f1)
HCD=$(print -r -- $HCS | sed -n 's/^CDHash=//p'); [[ $HCD =~ '^[0-9a-f]{40}$' ]] || die "can't read the camera helper's CDHash"

# 3. stage the installed layout. The Mach-O files go into payload.tar (cp/ditto keep the bytes, so the linker signatures stay
#    valid): Homebrew's keg relinking rewrites install names and re-signs loose Mach-O files under the prefix, never
#    inside an archive.
SH=$P/share/stuntcam; PL=$OUTD/payload
mkdir -p $PL/builds/$H $PL/stub/$S $PL/hook/$K $PL/canary $PL/helper/$HS $SH/profiles $SH/media $SH/restore-rules
cp build/$H/libsimcam.dylib build/$H/libsimcamloader.dylib $PL/builds/$H/
cp build/$S/libsimcamstub.dylib $PL/stub/$S/; cp build/$K/libsimcamrunnerhook.dylib $PL/hook/$K/
ditto --norsrc --noextattr canary/build/m10canary.app $PL/canary/m10canary.app
ditto --norsrc --noextattr "$HAPP" "$PL/helper/$HS/Stuntcam Camera.app"
cp engine/profiles/cfg-m10.plist $SH/profiles/; cp fixtures/media/qr.png $SH/media/
cp restore-rules/[0-9]*.json $SH/restore-rules/
cp LICENSE THIRD_PARTY_NOTICES.md $P/; cp packaging/npm/README.md $P/README.md
(cd $PL && find . -type f -exec chmod go-w {} + && find . -exec touch -h -t 198510260000 {} +)
# ustar, no xattrs/AppleDouble files, no owner names; members in sorted order (a stable tar for the same files)
(cd $PL && find builds stub hook canary helper -type f | LC_ALL=C sort > $OUTD/payload.list \
  && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --no-mac-metadata --no-acls --uid 0 --gid 0 --uname root --gname wheel \
     -cf $SH/payload.tar -T $OUTD/payload.list)
diff <(tar -tf $SH/payload.tar) $OUTD/payload.list || die "payload.tar doesn't list exactly the staged Mach-O files"
CSHA=$(shasum canary/build/m10canary.app/m10canary | cut -c1-8); PSHA=$(sha12 engine/profiles/cfg-m10.plist)
NV=0
if [[ -n $VERDICTS ]]; then
  [[ -d $VERDICTS ]] || die "--verdicts $VERDICTS: not a directory"
  mkdir -p $SH/verdicts
  NV=$(node - "$VERDICTS" "$SH/verdicts" "-$H-$S-c$CSHA-$PSHA" "$V" "$C" "$XB" <<'JS'
const fs = require('fs'), path = require('path');
const [src, dst, suffix, version, commit, xcode] = process.argv.slice(2);
let n = 0;
for (const f of fs.readdirSync(src).filter(f => f.endsWith('.json')).sort()) {
  let r; try { r = JSON.parse(fs.readFileSync(path.join(src, f), 'utf8')); } catch { continue; }
  // a plain pass for exactly this build: retried/relaunched/confirmed passes are short-lived by design, fails stay local
  if (r.verdict !== 'pass' || !r.key || f !== r.key + '.json' || !r.key.endsWith(suffix) || r.key !== r.runtime_build + suffix) continue;
  if (r.retried || r.relaunched || r.failed_first || /retried:\[/.test(r.result || '')) continue;
  const out = { proto: r.proto, key: r.key, verdict: r.verdict, result: r.result, runtime_build: r.runtime_build,
    runtime_version: r.runtime_version, engine_build: r.engine_build, stub_build: r.stub_build, canary_sha: r.canary_sha,
    profile_sha: r.profile_sha, device: '', at: r.at, packager_xcode: xcode,
    notes: [`shipped with stuntcam ${version} (commit ${commit}, packaged with Xcode ${xcode}): release-gate canary pass`] };
  fs.writeFileSync(path.join(dst, f), JSON.stringify(out, null, 1) + '\n'); n++;
}
console.log(n);
JS
)
  (( NV > 0 )) || die "--verdicts $VERDICTS: no plain canary pass for this build (*-$H-$S-c$CSHA-$PSHA)"
fi

node - "$P/package.json" "$V" <<'JS'
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

# the CLI checks every share/stuntcam line and every payload/ line at each use; the other lines are informational (Homebrew
# installs only bin/ and share/). bin/stuntcam isn't listed: it is built next, with this manifest's sha256 in it (and may
# be re-signed by Homebrew, which doesn't change the embedded values).
[[ ! -e $P/bin/stuntcam ]] || die "bin/stuntcam exists before the manifest"
{ print "stuntcam $V commit $C"; print "engine+loader $H stub $S hook $K canary $CSHA profile $PSHA verdicts $NV helper $HKIND $HS sha256 $HEXE cdhash $HCD xcode $XB"
  (cd $P && find . -type f ! -path ./share/stuntcam/manifest.txt | LC_ALL=C sort | sed 's|^\./||' | while read -r f; do print -r -- "$(shasum -a 256 "$f" | cut -d' ' -f1)  $f"; done)
  (cd $PL && while read -r f; do print -r -- "$(shasum -a 256 "$f" | cut -d' ' -f1)  payload/$f"; done < $OUTD/payload.list)
} > $SH/manifest.txt
# nothing loose under share/ is Mach-O (the point of the payload)
for f in $SH/**/*(.); do [[ $(file -b "$f") != Mach-O* ]] || die "${f#$P/} is a loose Mach-O file: it belongs in payload.tar"; done

# 3b. the CLI, built now that share/ is final: the sha256 of manifest.txt and payload.tar go into the binary (-X), so it
#     refuses a share/stuntcam it wasn't packaged with (integrity, and authenticity relative to the binary)
MSUM=$(shasum -a 256 $SH/manifest.txt | cut -d' ' -f1); PSUM=$(shasum -a 256 $SH/payload.tar | cut -d' ' -f1)
AP=github.com/avinav-b/stuntcam/cli/internal/artifacts
LDFLAGS_X="-X $AP.packagedManifest=$MSUM -X $AP.packagedPayload=$PSUM" GOARCHS=arm64 OUT=$OUTD/cli cli/build.sh > $OUTD/cli.log 2>&1 \
  || { cat $OUTD/cli.log; die "cli/build.sh failed"; }
[[ $($OUTD/cli/stuntcam version) == "stuntcam $V (commit $C)" ]] || die "CLI says '$($OUTD/cli/stuntcam version)', want 'stuntcam $V (commit $C)'"
[[ $(codesign -dv $OUTD/cli/stuntcam 2>&1) == *adhoc* ]] || die "CLI binary is not ad-hoc signed"   # no grep -q under pipefail: SIGPIPE
cp $OUTD/cli/stuntcam $P/bin/stuntcam && chmod 755 $P/bin/stuntcam
# the staged package checks itself with the embedded hashes, from a scratch host dir (and from outside any checkout)
CK=$OUTD/check-home; rm -rf $CK
out=$(cd / && SIMCAM_HOME=$CK $P/bin/stuntcam version --check 2>&1) || { print -r -- "$out"; die "version --check of the staged package failed"; }
[[ $out == *"packaged with (manifest sha256 ${MSUM[1,12]})"* ]] || { print -r -- "$out"; die "the staged CLI doesn't carry the manifest's sha256"; }
# the helper the CLI unpacked from the tar still verifies (exec bits and every sealed file survived the round trip)
codesign --verify --strict "$CK/helper/$HS/Stuntcam Camera.app" || die "the camera helper unpacked from payload.tar doesn't verify"
rm -rf $CK

# 4. no build-machine paths, no secrets, nothing unexpected
# fixed strings (-F: a path with regex characters still matches itself); grep exits 0 = found, 1 = none, 2 = error (a
# failure too, never "clean"). payload.tar is uncompressed, so its members are scanned as well (and unpacked, below).
scan() {  # scan <what> <grep args…>: die if any file in $P (or the unpacked payload) matches
  local what=$1 out rc; shift
  out=$(LC_ALL=C grep -rla "$@" $P $PL) && rc=0 || rc=$?
  (( rc == 1 )) || { (( rc == 0 )) && die "$what in: $out"; die "grep failed (exit $rc) scanning for $what"; }
}
scan "build-machine paths" -F -e "$ROOT" -e "$LROOT" -e "$HOME" -e /Users/ -e /private/var/folders -e "$(hostname -s).local" -e stuntcam-work
scan "secret-looking strings" -E -e 'BEGIN [A-Z ]*PRIVATE KEY|gh[pousr]_[A-Za-z0-9]{30,}|npm_[A-Za-z0-9]{30,}|AKIA[0-9A-Z]{16}|_authToken'

# 5. npm pack (+ the dry-run file list), then check the tarball against the staged tree
(cd $OUTD && npm pack ./stuntcam --dry-run 2>&1 | sed -n '/Tarball Contents/,/Tarball Details/p' > filelist.txt)
(cd $OUTD && npm pack ./stuntcam --pack-destination . > pack.log 2>&1) || { cat $OUTD/pack.log; die "npm pack failed"; }
TGZ=$OUTD/stuntcam-$V.tgz; [[ -f $TGZ ]] || die "no $TGZ"
diff <(tar -tzf $TGZ | sed 's|^package/||' | grep -v '/$' | LC_ALL=C sort) <(cd $P && find . -type f | sed 's|^\./||' | LC_ALL=C sort) \
  || die "tarball and staged tree differ"
[[ $(tar -tvzf $TGZ package/bin/stuntcam) == -rwxr-xr-x* ]] || die "bin/stuntcam lost its exec bit in the tarball"
[[ $(tar -tvf $SH/payload.tar canary/m10canary.app/m10canary) == -rwxr-xr-x* ]] || die "the canary lost its exec bit in payload.tar"
for f in "$PL/helper/$HS/Stuntcam Camera.app"/Contents/MacOS/*(N); do
  [[ $(tar -tvf $SH/payload.tar "${f#$PL/}") == -rwxr-xr-x* ]] || die "${f#$PL/} lost its exec bit in payload.tar"
done
SUM=$(shasum -a 256 $TGZ | cut -d' ' -f1)

# 6. the Homebrew formula and the GitHub action (generated here, never committed; not published; the URL is a placeholder)
cat > $OUTD/stuntcam.rb <<RB
# SPDX-License-Identifier: Apache-2.0
# Generated by scripts/package.sh from stuntcam-$V.tgz (commit $C). NOT PUBLISHED: no tap exists yet.
# The tarball is the npm package; Homebrew strips its top "package/" dir. bin/stuntcam finds share/stuntcam next to it
# (cli/internal/artifacts: the installed layout). The simulator's Mach-O files ship inside share/stuntcam/payload.tar,
# so Homebrew's keg relinking (install names, re-signing) never touches them; the CLI checks every file against
# share/stuntcam/manifest.txt and unpacks the payload into \$SIMCAM_HOME at each use.
class Stuntcam < Formula
  desc "Camera for the iOS Simulator: real AVFoundation, no app changes"
  homepage "https://stuntcam.dev"
  # TODO(release): placeholder URL. Upload stuntcam-$V.tgz to the v$V release (or point at the npm registry tarball,
  # https://registry.npmjs.org/stuntcam/-/stuntcam-$V.tgz, once published) and keep sha256 in sync with that file.
  url "https://github.com/avinav-b/stuntcam/releases/download/v$V/stuntcam-$V.tgz"
  sha256 "$SUM"
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
mkdir -p $OUTD/setup-stuntcam
sed "s/@VERSION@/$V/g" packaging/setup-stuntcam/action.template.yml | sed '/^# setup-stuntcam (design/,/^# a public repo/c\
# setup-stuntcam (design §10, CI), generated by scripts/package.sh for stuntcam '"$V"' (commit '"$C"'). NOT PUBLISHED.
' > $OUTD/setup-stuntcam/action.yml
cp packaging/setup-stuntcam/README.md $OUTD/setup-stuntcam/
[[ $(<$OUTD/setup-stuntcam/action.yml) != *@VERSION@* ]] || die "action.yml: @VERSION@ left"

print "version $V commit $C"
print "manifest sha256 $MSUM  payload sha256 $PSUM (embedded in bin/stuntcam)"
print "engine+loader $H  stub $S (frozen)  hook $K  canary $CSHA  profile $PSHA  shipped verdicts $NV"
print "camera helper $HS ($HKIND, $HID, CDHash $HCD)"
print "package dir: $P"
print "tarball: $TGZ ($(stat -f %z $TGZ) bytes, unpacked $(du -sk $P | cut -f1) KiB, $(tar -tzf $TGZ | grep -vc '/$') files)"
print "sha256 $SUM"
print "formula: $OUTD/stuntcam.rb"
print "action: $OUTD/setup-stuntcam/action.yml"
