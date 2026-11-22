// SPDX-License-Identifier: Apache-2.0
package artifacts

import (
	"archive/tar"
	"bufio"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

const (
	ManifestName = "manifest.txt" // share/stuntcam/manifest.txt: version, build ids, sha256 of every file
	PayloadName  = "payload.tar"  // share/stuntcam/payload.tar: every Mach-O file the simulator runs, byte for byte
	payloadPre   = "payload/"     // manifest paths of the files inside payload.tar
	sharePre     = "share/stuntcam/"
)

// Installed resolves the installed layout under prefix (the directory holding bin/stuntcam): what scripts/package.sh
// writes into the npm package, and what the Homebrew formula installs into its keg:
//
//	share/stuntcam/manifest.txt                     "stuntcam <version> commit <c>", the build ids, and one
//	                                                "<sha256>  <path>" line per file, payload members as payload/<member>
//	share/stuntcam/payload.tar                      uncompressed tar of the Mach-O files the simulator loads or runs:
//	    builds/<pair>/{libsimcam,libsimcamloader}.dylib   pair = sha1(engine bytes + loader bytes), 12 hex
//	    stub/<sha1-12>/libsimcamstub.dylib                the frozen stub
//	    hook/<sha1-12>/libsimcamrunnerhook.dylib          the XCTest runner hook (exec)
//	    canary/m10canary.app/…                            the prebuilt canary
//	    helper/<sha>/Stuntcam Camera.app/…                the camera helper (signed; <sha>: helperBundleSHA1 of its files)
//	share/stuntcam/profiles/cfg-m10.plist, media/qr.png, restore-rules/<xcode build>.json, verdicts/<key>.json (optional)
//
// The Mach-O files ship inside the tar because package managers rewrite loose ones. Homebrew's keg relinking changes
// install names and re-signs. That drops the linker signature that simulator processes rely on, and breaks the
// content-hash names. Nothing rewrites inside an archive.
//
// Every use checks the files:
//   - every file under share/stuntcam is checked against the manifest. An extra, missing or changed file is an error;
//   - each payload member is checked against it too;
//   - the dylibs are unpacked into home.Builds()/<hash>/, where Stage puts them anyway, and the canary into
//     home.Canary()/<id>/. Copies already there are checked again.
//
// A changed file is an error, never a silent arm.
//
// A packaged binary also carries the sha256 of the manifest and of payload.tar (packagedManifest and packagedPayload,
// set by scripts/package.sh at link time). So the manifest is anchored to bin/stuntcam, and replacing payload.tar and
// rewriting manifest.txt to match is refused too. The check gives integrity, and authenticity relative to the binary:
// share/ is exactly what was packaged with this bin/stuntcam.
// Whoever can replace bin/stuntcam itself can replace everything. The binary's own authenticity comes from how it was
// installed: the npm registry, or the formula's sha256.
// A binary without the values, such as a dev build or `go build`, checks only the manifest's internal consistency.
func Installed(prefix string) (Set, error) {
	share := filepath.Join(prefix, "share", "stuntcam")
	if !exists(share) {
		return Set{}, errNoCheckout
	}

	set := Set{From: share, Installed: true}
	if err := checkAnchor(filepath.Join(share, ManifestName)); err != nil {
		return set, err
	}

	shareManifest, err := readManifest(filepath.Join(share, ManifestName))
	if err != nil {
		return set, fmt.Errorf("installed stuntcam is incomplete: %w; reinstall stuntcam", err)
	}

	set.Version = shareManifest.version
	if packagedPayload != "" && shareManifest.files[sharePre+PayloadName] != packagedPayload {
		listed := ShortHash(shareManifest.files[sharePre+PayloadName])
		return set, notPackaged(fmt.Sprintf("%s lists %s with sha256 %s", ManifestName, PayloadName, listed),
			packagedPayload)
	}

	if err := shareManifest.checkShare(share); err != nil {
		return set, err
	}

	payload, err := shareManifest.unpack(filepath.Join(share, PayloadName))
	if err != nil {
		return set, err
	}

	set.Loader = payload.loader
	set.Engine = payload.engine
	set.Stub = payload.stub
	set.Hook = payload.hook
	set.Canary = payload.canary
	set.Helper = payload.helper
	set.Profile, set.Image = dataFiles(share)
	set.Rules = filepath.Join(share, "restore-rules")
	if v := filepath.Join(share, "verdicts"); exists(v) {
		set.Verdicts = v
	}

	if err := set.check(); err != nil {
		return set, err
	}

	if set.Canary == "" {
		return set, fmt.Errorf("installed stuntcam is incomplete: no canary in %s; reinstall stuntcam", PayloadName)
	}

	if set.Profile == "" || set.Image == "" {
		return set, fmt.Errorf("installed stuntcam is incomplete: no profiles/cfg-m10.plist or media/qr.png in %s; "+
			"reinstall stuntcam", share)
	}

	return set, nil
}

// packagedManifest and packagedPayload are the sha256 (hex) of share/stuntcam/manifest.txt and payload.tar this binary
// was packaged with: scripts/package.sh builds the CLI after staging them and passes -ldflags "-X
// github.com/avinav-b/stuntcam/cli/internal/artifacts.packagedManifest=<sha256> -X ….packagedPayload=<sha256>".
// Empty in a dev build.
var packagedManifest, packagedPayload string

// Anchored reports whether this binary carries the package's hashes (a packaged build), and the manifest's.
func Anchored() (bool, string) { return packagedManifest != "", packagedManifest }

// ShortHash is a hash as messages quote it: its first hashLen hex digits.
func ShortHash(h string) string {
	if len(h) > hashLen {
		return h[:hashLen]
	}
	return h
}

func notPackaged(what, want string) error {
	return fmt.Errorf("installed stuntcam doesn't match its binary: %s, but bin/stuntcam was packaged with %s "+
		"(share/stuntcam was replaced after packaging, or bin/stuntcam comes from another package); reinstall stuntcam",
		what, ShortHash(want))
}

// checkAnchor checks that manifest.txt is the one this binary was packaged with (it does nothing in a dev build).
func checkAnchor(manifestPath string) error {
	if packagedManifest == "" {
		return nil
	}

	have, err := sha256File(manifestPath)
	if err != nil {
		return fmt.Errorf("installed stuntcam is incomplete: no %s; reinstall stuntcam", ManifestName)
	}

	if have != packagedManifest {
		return notPackaged(fmt.Sprintf("%s has sha256 %s", ManifestName, ShortHash(have)), packagedManifest)
	}

	return nil
}

// ShippedVerdicts lists the shipped verdict files (none outside the installed layout).
func (s Set) ShippedVerdicts() []string {
	if s.Verdicts == "" {
		return nil
	}
	m, _ := filepath.Glob(filepath.Join(s.Verdicts, "*.json"))
	return m
}

type manifest struct {
	version string            // "1.0.0 (commit abc)" from the first line
	files   map[string]string // path -> sha256 hex, as listed
}

var manifestLine = regexp.MustCompile(`^([0-9a-f]{64})  (\S.*)$`)

func readManifest(manifestPath string) (manifest, error) {
	file, err := os.Open(manifestPath)
	if err != nil {
		return manifest{}, fmt.Errorf("no %s", ManifestName)
	}
	defer file.Close()

	parsed := manifest{files: map[string]string{}}
	scanner := bufio.NewScanner(file)
	for lineNo := 1; scanner.Scan(); lineNo++ {
		line := scanner.Text()
		if lineNo == 1 {
			versionText, commit, ok := strings.Cut(strings.TrimPrefix(line, "stuntcam "), " commit ")
			if !strings.HasPrefix(line, "stuntcam ") || !ok {
				return manifest{}, fmt.Errorf("%s: line 1 is not \"stuntcam <version> commit <commit>\"", ManifestName)
			}

			parsed.version = versionText + " (commit " + commit + ")"
			continue
		}

		match := manifestLine.FindStringSubmatch(line)
		if match == nil {
			continue // the build-id line
		}

		if _, dup := parsed.files[match[2]]; dup || !safeRel(match[2]) {
			return manifest{}, fmt.Errorf("%s: bad entry %q", ManifestName, match[2])
		}

		parsed.files[match[2]] = match[1]
	}

	if err := scanner.Err(); err != nil {
		return manifest{}, err
	}

	if parsed.version == "" || parsed.files[sharePre+PayloadName] == "" {
		return manifest{}, fmt.Errorf("%s lists no %s", ManifestName, PayloadName)
	}

	return parsed, nil
}

// safeRel reports whether p is a clean relative slash path without "..".
func safeRel(p string) bool {
	return p != "" && p != "." && p != ".." && path.Clean(p) == p &&
		!strings.HasPrefix(p, "/") && !strings.HasPrefix(p, "../") && !strings.Contains(p, "/../")
}

func sha256File(p string) (string, error) {
	f, err := os.Open(p)
	if err != nil {
		return "", err
	}
	defer f.Close()

	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}

	return hex.EncodeToString(h.Sum(nil)), nil
}

func changed(what string) error {
	return fmt.Errorf("installed stuntcam files changed after packaging: %s (something rewrote or added files after "+
		"install, e.g. relinking or re-signing); reinstall stuntcam", what)
}

// checkShare checks that every regular file under share (except the manifest and Finder's .DS_Store files) is listed
// with its sha256, and that every listed share file is there.
func (m manifest) checkShare(share string) error {
	seen := map[string]bool{}
	err := filepath.WalkDir(share, func(walked string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}

		rel, _ := filepath.Rel(share, walked) // can't fail: walked is under share
		rel = filepath.ToSlash(rel)
		if entry.IsDir() || rel == ManifestName {
			return nil
		}

		if entry.Name() == ".DS_Store" && entry.Type().IsRegular() {
			return nil // Finder writes one into any folder a user opens; it never holds anything stuntcam reads
		}

		if !entry.Type().IsRegular() {
			return changed(rel + " is not a regular file")
		}

		want, ok := m.files[sharePre+rel]
		if !ok {
			return changed(rel + " is not in " + ManifestName)
		}

		have, err := sha256File(walked)
		if err != nil {
			return err
		}

		if have != want {
			return changed(fmt.Sprintf("%s has sha256 %s, the manifest says %s", rel, have[:hashLen], want[:hashLen]))
		}

		seen[sharePre+rel] = true
		return nil
	})

	if err != nil {
		return err
	}

	for p := range m.files {
		if strings.HasPrefix(p, sharePre) && !seen[p] {
			return changed(strings.TrimPrefix(p, sharePre) + " is missing")
		}
	}

	return nil
}

type unpacked struct{ loader, engine, stub, hook, canary, helper string }

// member is one payload file: its bytes and whether the tar marks it executable.
type member struct {
	data []byte
	exec bool
}

var (
	payloadDylib  = regexp.MustCompile(`^(builds|stub|hook)/([0-9a-f]{12})/([A-Za-z0-9_.]+\.dylib)$`)
	payloadCanary = regexp.MustCompile(`^canary/m10canary\.app/[A-Za-z0-9_./ -]+$`)
	payloadHelper = regexp.MustCompile(`^helper/([0-9a-f]{12})/(Stuntcam Camera\.app/[A-Za-z0-9_./ +-]+)$`)
)

// HelperApp is the camera helper bundle's name; HelperExe its executable inside it.
const (
	HelperApp = "Stuntcam Camera.app"
	HelperExe = "Contents/MacOS/stuntcam-camera"
)

// helperBundleSHA1 is a bundle's <sha> as helper/build.sh computes it: the first 12 hex digits of the sha1 over one
// "<path relative to the bundle> <sha1 hex>\n" line per file, in byte order of the paths.
func helperBundleSHA1(files map[string][]byte) string {
	var rels []string
	for r := range files {
		rels = append(rels, r)
	}

	sort.Strings(rels)
	h := sha1.New()
	for _, r := range rels {
		fmt.Fprintf(h, "%s %x\n", r, sha1.Sum(files[r]))
	}

	return hex.EncodeToString(h.Sum(nil))[:hashLen]
}

// HelperSHA is the <sha> directory the set's camera helper sits in, or "".
func (s Set) HelperSHA() string {
	if s.Helper == "" {
		return ""
	}
	return filepath.Base(filepath.Dir(s.Helper))
}

// maxPayloadMember bounds how much of one payload member is read: far above any file stuntcam ships (a bigger member is
// cut short and then fails its sha256 check).
const maxPayloadMember = 64 << 20

// unpack reads payload.tar (already checked against the manifest by checkShare), checks that its members are exactly
// the manifest's payload/ entries with those sha256s, and puts them under the host directory: dylibs at
// home.Builds()/<hash>/<name> (their dir names checked against their bytes, as ./build.sh names them), the canary at
// home.Canary()/<id>/m10canary.app, the camera helper at home.Root()/helper/<sha>/. Copies already there are re-checked
// and rewritten if they differ.
func (m manifest) unpack(tarPath string) (unpacked, error) {
	var result unpacked
	want := m.payloadSums()
	got, err := readPayload(tarPath, want)
	if err != nil {
		return result, err
	}

	groups, err := groupPayload(got)
	if err != nil {
		return result, err
	}

	for dir, names := range groups.dylibDirs {
		if err := result.unpackDylibDir(dir, names, got); err != nil {
			return result, err
		}
	}

	if len(groups.canary) > 0 {
		if result.canary, err = unpackCanary(groups.canary, got, want); err != nil {
			return result, err
		}
	}

	if len(groups.helpers) > 1 {
		return result, changed(PayloadName + " has more than one camera helper")
	}

	for sha, names := range groups.helpers {
		if result.helper, err = unpackHelper(sha, names, got, want); err != nil {
			return result, err
		}
	}

	return result, nil
}

// payloadSums is the manifest's payload/ entries: member name -> sha256.
func (m manifest) payloadSums() map[string]string {
	want := map[string]string{}
	for p, h := range m.files {
		if strings.HasPrefix(p, payloadPre) {
			want[strings.TrimPrefix(p, payloadPre)] = h
		}
	}
	return want
}

// readPayload reads every member of the payload tar and checks the members are exactly want's, with those sha256s.
func readPayload(tarPath string, want map[string]string) (map[string]member, error) {
	file, err := os.Open(tarPath)
	if err != nil {
		return nil, err
	}
	defer file.Close()

	got := map[string]member{}
	reader := tar.NewReader(file)
	for {
		header, err := reader.Next()
		if err == io.EOF {
			break
		}

		if err != nil {
			return nil, changed(PayloadName + " is not a readable tar: " + err.Error())
		}

		name := strings.TrimPrefix(header.Name, "./")
		if header.Typeflag == tar.TypeDir {
			continue
		}

		if header.Typeflag != tar.TypeReg || !safeRel(name) {
			return nil, changed(fmt.Sprintf("%s has an unexpected member %q", PayloadName, header.Name))
		}

		wantSum, ok := want[name]
		if !ok {
			return nil, changed(fmt.Sprintf("%s member %s is not in %s", PayloadName, name, ManifestName))
		}

		if _, dup := got[name]; dup {
			return nil, changed(fmt.Sprintf("%s has %s twice", PayloadName, name))
		}

		data, err := io.ReadAll(io.LimitReader(reader, maxPayloadMember))
		if err != nil {
			return nil, err
		}

		if sum := sha256.Sum256(data); hex.EncodeToString(sum[:]) != wantSum {
			return nil, changed(fmt.Sprintf("%s member %s doesn't match %s", PayloadName, name, ManifestName))
		}

		got[name] = member{data, header.Mode&0o111 != 0}
	}

	for name := range want {
		if _, ok := got[name]; !ok {
			return nil, changed(fmt.Sprintf("%s has no %s", PayloadName, name))
		}
	}

	return got, nil
}

// payloadGroups is the payload's members by what they are.
type payloadGroups struct {
	dylibDirs map[string][]string // "builds/<h>", "stub/<h>", "hook/<h>" -> member names
	canary    []string            // canary/m10canary.app/…
	helpers   map[string][]string // <sha> -> member names under helper/<sha>/
}

// groupPayload sorts the members into dylib dirs, the canary and camera helpers; any other member is an error.
func groupPayload(got map[string]member) (payloadGroups, error) {
	groups := payloadGroups{dylibDirs: map[string][]string{}, helpers: map[string][]string{}}
	for name := range got {
		if dylibMatch := payloadDylib.FindStringSubmatch(name); dylibMatch != nil {
			dir := dylibMatch[1] + "/" + dylibMatch[2]
			groups.dylibDirs[dir] = append(groups.dylibDirs[dir], name)
		} else if payloadCanary.MatchString(name) {
			groups.canary = append(groups.canary, name)
		} else if helperMatch := payloadHelper.FindStringSubmatch(name); helperMatch != nil {
			groups.helpers[helperMatch[1]] = append(groups.helpers[helperMatch[1]], name)
		} else {
			return groups, changed(fmt.Sprintf("%s has an unexpected member %s", PayloadName, name))
		}
	}

	return groups, nil
}

// unpackDylibDir checks a dylib dir's name against its members' bytes (in the order ./build.sh hashes them: engine,
// then loader) and writes them to home.Builds()/<hash>/, recording each in u.
func (u *unpacked) unpackDylibDir(dir string, names []string, got map[string]member) error {
	kind, dirHash := path.Dir(dir), path.Base(dir)
	var order []string
	switch kind {
	case "builds":
		order = []string{dir + "/" + EngineName, dir + "/" + LoaderName}
	case "stub":
		order = []string{dir + "/" + StubName}
	case "hook":
		order = []string{dir + "/" + HookName}
	}

	if len(names) != len(order) {
		return changed(fmt.Sprintf("%s: %s holds %v", PayloadName, dir, names))
	}

	hasher := sha1.New()
	for _, name := range order {
		found, ok := got[name]
		if !ok {
			return changed(fmt.Sprintf("%s has no %s", PayloadName, name))
		}

		hasher.Write(found.data)
	}

	if sum := hex.EncodeToString(hasher.Sum(nil))[:hashLen]; sum != dirHash {
		return changed(fmt.Sprintf("%s hashes to %s, not %s", dir, sum, dirHash))
	}

	for _, name := range order {
		dst := filepath.Join(home.Builds(), dirHash, path.Base(name))
		if err := writeVerified(dst, got[name].data); err != nil {
			return err
		}

		if err := u.record(path.Base(name), dst); err != nil {
			return err
		}
	}

	return nil
}

// record notes where a dylib was unpacked; a second engine, stub or runner hook is an error.
func (u *unpacked) record(name, dst string) error {
	switch name {
	case EngineName:
		if u.engine != "" {
			return changed(PayloadName + " has more than one engine")
		}

		u.engine = dst
	case LoaderName:
		u.loader = dst
	case StubName:
		if u.stub != "" {
			return changed(PayloadName + " has more than one stub")
		}

		u.stub = dst
	case HookName:
		if u.hook != "" {
			return changed(PayloadName + " has more than one runner hook")
		}

		u.hook = dst
	}

	return nil
}

// unpackCanary puts the canary's members into home.Canary()/<id>/, id the first 12 hex digits of the sha256 over their
// manifest lines, and returns the unpacked m10canary.app.
func unpackCanary(names []string, got map[string]member, want map[string]string) (string, error) {
	sort.Strings(names)
	id := sha256.New()
	for _, n := range names {
		fmt.Fprintf(id, "%s  %s\n", want[n], n)
	}

	files := map[string]member{}
	for _, n := range names {
		files[strings.TrimPrefix(n, "canary/")] = got[n]
	}

	dir := filepath.Join(home.Canary(), hex.EncodeToString(id.Sum(nil))[:hashLen])
	if err := unpackTree(dir, files, func(rel string) string { return want["canary/"+rel] }); err != nil {
		return "", err
	}

	return filepath.Join(dir, "m10canary.app"), nil
}

// unpackHelper checks the camera helper's dir name against its bundle's content hash (helperBundleSHA1) and its
// executable's exec bit (upload-artifact and npm drop exec bits elsewhere, a tar keeps them), puts it into
// home.Root()/helper/<sha>/ and returns the unpacked app.
func unpackHelper(sha string, names []string, got map[string]member, want map[string]string) (string, error) {
	files, data := map[string]member{}, map[string][]byte{}
	for _, n := range names {
		rel := strings.TrimPrefix(n, "helper/"+sha+"/")
		files[rel] = got[n]
		data[strings.TrimPrefix(rel, HelperApp+"/")] = got[n].data
	}

	if sum := helperBundleSHA1(data); sum != sha {
		return "", changed(fmt.Sprintf("%s: helper/%s hashes to %s", PayloadName, sha, sum))
	}

	if exe, ok := files[HelperApp+"/"+HelperExe]; !ok || !exe.exec {
		return "", changed(fmt.Sprintf("%s: helper/%s has no executable %s", PayloadName, sha, HelperExe))
	}

	dir := filepath.Join(home.Root(), "helper", sha)
	if err := unpackTree(dir, files, func(rel string) string { return want["helper/"+sha+"/"+rel] }); err != nil {
		return "", err
	}

	return filepath.Join(dir, HelperApp), nil
}

// writeVerified makes dst hold data (0644, atomically), unless it already does.
func writeVerified(dst string, data []byte) error {
	if err := putIdentical(dst, data); err != nil {
		if errors.Is(err, errNotIdentical) {
			return fmt.Errorf("unpacked %s is not identical", dst)
		}
		return err
	}
	return nil
}

// unpackTree makes dir hold exactly files, a map from relative path to bytes and exec bit.
//   - A dir that already matches is kept, and its mtime is set to now, because gc's "newest" goes by it.
//   - Otherwise the tree is written to a temp dir that then replaces dir, so a canary install never reads half a
//     bundle.
//
// The check, the staging and the swap run under an flock on <parent>/.lock. Two stuntcams can unpack at once, as with
// two commands right after an install. They never rename away each other's dir: the second one waits, checks again,
// and keeps the dir the first one wrote. A dir that matches is never renamed away.
func unpackTree(dir string, files map[string]member, sum func(rel string) string) error {
	if treeMatches(dir, files, sum) {
		now := time.Now()
		_ = os.Chtimes(dir, now, now) // only freshens the age gc goes by
		return nil
	}

	parent := filepath.Dir(dir)
	unlock, err := LockDir(parent)
	if err != nil {
		return err
	}
	defer unlock()

	if treeMatches(dir, files, sum) {
		return nil // unpacked by another stuntcam while this one waited for the lock
	}

	tmp, err := os.MkdirTemp(parent, ".stage-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)

	if err := os.Chmod(tmp, 0o755); err != nil {
		return err
	}

	for rel, file := range files {
		dst := filepath.Join(tmp, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			return err
		}

		mode := os.FileMode(0o644)
		if file.exec {
			mode = 0o755
		}

		if err := os.WriteFile(dst, file.data, mode); err != nil {
			return err
		}

		if err := os.Chmod(dst, mode); err != nil {
			return err
		}
	}

	if _, err := os.Lstat(dir); err == nil { // a changed copy (checked above, under the lock): move it aside, then swap
		old, err := os.MkdirTemp(parent, ".old-")
		if err != nil {
			return err
		}

		if err := os.Remove(old); err != nil {
			return err
		}

		if err := os.Rename(dir, old); err != nil {
			return err
		}
		defer os.RemoveAll(old)
	}

	return os.Rename(tmp, dir)
}

// LockDir takes an exclusive flock on dir/.lock (dir and the file are created if needed) and returns its release. The
// lock goes with the process: a stuntcam that dies holding it never blocks the next one.
func LockDir(dir string) (func(), error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}

	// read-only: flock works on it, and another user's .lock stays usable
	lockFile, err := os.OpenFile(filepath.Join(dir, ".lock"), os.O_CREATE|os.O_RDONLY, 0o644)
	if err != nil {
		return nil, err
	}

	for {
		err = syscall.Flock(int(lockFile.Fd()), syscall.LOCK_EX)
		if err != syscall.EINTR {
			break
		}
	}

	if err == syscall.ENOTSUP || err == syscall.EOPNOTSUPP || err == syscall.ENOLCK { // a filesystem without locks: go on
		lockFile.Close()
		return func() {}, nil
	}

	if err != nil {
		lockFile.Close()
		return nil, fmt.Errorf("lock %s: %w", lockFile.Name(), err)
	}

	unlock := func() {
		syscall.Flock(int(lockFile.Fd()), syscall.LOCK_UN) // the Close below releases it anyway
		lockFile.Close()
	}

	return unlock, nil
}

// treeMatches reports whether dir is a directory holding exactly the regular files in files, each with sha256 sum(its
// relative path) and the exec bit files says (a chmod 644'd canary executable doesn't match, so it is unpacked
// again).
func treeMatches(dir string, files map[string]member, sum func(rel string) string) bool {
	if fi, err := os.Lstat(dir); err != nil || !fi.IsDir() {
		return false
	}

	count := 0
	bad := errors.New("differs")
	err := filepath.WalkDir(dir, func(walked string, entry fs.DirEntry, err error) error {
		if err != nil {
			return err
		}

		if entry.IsDir() {
			return nil
		}

		rel, _ := filepath.Rel(dir, walked) // can't fail: walked is under dir
		rel = filepath.ToSlash(rel)
		want, ok := files[rel]
		if !ok || !entry.Type().IsRegular() {
			return bad
		}

		info, err := entry.Info()
		if err != nil || (info.Mode().Perm()&0o111 != 0) != want.exec {
			return bad
		}

		if h, err := sha256File(walked); err != nil || h != sum(rel) {
			return bad
		}

		count++
		return nil
	})

	return err == nil && count == len(files)
}
