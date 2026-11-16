// SPDX-License-Identifier: Apache-2.0
package artifacts

import (
	"archive/tar"
	"bufio"
	"bytes"
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
//	    helper/<sha>/Stuntcam Camera.app/…                the camera helper (signed; <sha>: BundleSHA of its files)
//	share/stuntcam/profiles/cfg-m10.plist, media/qr.png, restore-rules/<xcode build>.json, verdicts/<key>.json (optional)
//
// The Mach-O files ship inside the tar because package managers rewrite loose ones: Homebrew's keg relinking changes
// install names and re-signs, which drops the linker signature simulator processes rely on and the content-hash names.
// Nothing rewrites inside an archive. Every use checks every file under share/stuntcam against the manifest (an extra,
// missing or changed file is an error), checks each payload member against it, and unpacks the dylibs into
// home.Builds()/<hash>/ (where Stage puts them anyway) and the canary into home.Canary()/<id>/, re-checking the copies
// already there. A changed file is an error, never a silent arm.
//
// A packaged binary also carries the sha256 of the manifest and of payload.tar (packagedManifest, packagedPayload, set by
// scripts/package.sh at link time), so the manifest is anchored to bin/stuntcam: replacing payload.tar and re-writing
// manifest.txt to match is refused too. That makes the check integrity plus authenticity relative to the binary: share/
// is exactly what was packaged with this bin/stuntcam. Whoever can replace bin/stuntcam itself can replace everything;
// the binary's own authenticity comes from how it was installed (the npm registry, the formula's sha256). A binary
// without the values (a dev build, `go build`) checks the manifest's internal consistency only, as before.
func Installed(prefix string) (Set, error) {
	share := filepath.Join(prefix, "share", "stuntcam")
	if !exists(share) {
		return Set{}, errNoCheckout
	}
	s := Set{From: share, Installed: true}
	if err := checkAnchor(filepath.Join(share, ManifestName)); err != nil {
		return s, err
	}
	m, err := readManifest(filepath.Join(share, ManifestName))
	if err != nil {
		return s, fmt.Errorf("installed stuntcam is incomplete: %v; reinstall stuntcam", err)
	}
	s.Version = m.version
	if packagedPayload != "" && m.files[sharePre+PayloadName] != packagedPayload {
		return s, notPackaged(fmt.Sprintf("%s lists %s with sha256 %s", ManifestName, PayloadName, short(m.files[sharePre+PayloadName])), packagedPayload)
	}
	if err := m.checkShare(share); err != nil {
		return s, err
	}
	payload, err := m.unpack(filepath.Join(share, PayloadName))
	if err != nil {
		return s, err
	}
	s.Loader, s.Engine, s.Stub, s.Hook, s.Canary, s.Helper = payload.loader, payload.engine, payload.stub, payload.hook, payload.canary, payload.helper
	s.Profile, s.Image = dataFiles(share)
	s.Rules = filepath.Join(share, "restore-rules")
	if v := filepath.Join(share, "verdicts"); exists(v) {
		s.Verdicts = v
	}
	if err := s.check(); err != nil {
		return s, err
	}
	if s.Canary == "" {
		return s, fmt.Errorf("installed stuntcam is incomplete: no canary in %s; reinstall stuntcam", PayloadName)
	}
	if s.Profile == "" || s.Image == "" {
		return s, fmt.Errorf("installed stuntcam is incomplete: no profiles/cfg-m10.plist or media/qr.png in %s; reinstall stuntcam", share)
	}
	return s, nil
}

// packagedManifest and packagedPayload are the sha256 (hex) of share/stuntcam/manifest.txt and payload.tar this binary was
// packaged with: scripts/package.sh builds the CLI after staging them and passes
// -ldflags "-X github.com/avinav-b/stuntcam/cli/internal/artifacts.packagedManifest=<sha256> -X ….packagedPayload=<sha256>".
// Empty in a dev build.
var packagedManifest, packagedPayload string

// Anchored reports whether this binary carries the package's hashes (a packaged build), and the manifest's.
func Anchored() (bool, string) { return packagedManifest != "", packagedManifest }

func short(h string) string {
	if len(h) > 12 {
		return h[:12]
	}
	return h
}

func notPackaged(what, want string) error {
	return fmt.Errorf("installed stuntcam doesn't match its binary: %s, but bin/stuntcam was packaged with %s (share/stuntcam was replaced after packaging, or bin/stuntcam comes from another package); reinstall stuntcam", what, short(want))
}

// checkAnchor: manifest.txt is the one this binary was packaged with (no-op in a dev build).
func checkAnchor(manifestPath string) error {
	if packagedManifest == "" {
		return nil
	}
	have, err := sha256File(manifestPath)
	if err != nil {
		return fmt.Errorf("installed stuntcam is incomplete: no %s; reinstall stuntcam", ManifestName)
	}
	if have != packagedManifest {
		return notPackaged(fmt.Sprintf("%s has sha256 %s", ManifestName, short(have)), packagedManifest)
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

func readManifest(p string) (manifest, error) {
	f, err := os.Open(p)
	if err != nil {
		return manifest{}, fmt.Errorf("no %s", ManifestName)
	}
	defer f.Close()
	m := manifest{files: map[string]string{}}
	sc := bufio.NewScanner(f)
	for n := 1; sc.Scan(); n++ {
		l := sc.Text()
		if n == 1 {
			v, c, ok := strings.Cut(strings.TrimPrefix(l, "stuntcam "), " commit ")
			if !strings.HasPrefix(l, "stuntcam ") || !ok {
				return manifest{}, fmt.Errorf("%s: line 1 is not \"stuntcam <version> commit <commit>\"", ManifestName)
			}
			m.version = v + " (commit " + c + ")"
			continue
		}
		g := manifestLine.FindStringSubmatch(l)
		if g == nil {
			continue // the build-id line
		}
		if _, dup := m.files[g[2]]; dup || !safeRel(g[2]) {
			return manifest{}, fmt.Errorf("%s: bad entry %q", ManifestName, g[2])
		}
		m.files[g[2]] = g[1]
	}
	if err := sc.Err(); err != nil {
		return manifest{}, err
	}
	if m.version == "" || m.files[sharePre+PayloadName] == "" {
		return manifest{}, fmt.Errorf("%s lists no %s", ManifestName, PayloadName)
	}
	return m, nil
}

// safeRel: a clean relative slash path without "..".
func safeRel(p string) bool {
	return p != "" && !strings.HasPrefix(p, "/") && path.Clean(p) == p && p != "." && !strings.HasPrefix(p, "../") && !strings.Contains(p, "/../") && p != ".."
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
	return fmt.Errorf("installed stuntcam files changed after packaging: %s (something rewrote or added files after install, e.g. relinking or re-signing); reinstall stuntcam", what)
}

// checkShare: every regular file under share (but the manifest, and Finder's .DS_Store files) is listed with its sha256,
// and every listed share file is there.
func (m manifest) checkShare(share string) error {
	seen := map[string]bool{}
	err := filepath.WalkDir(share, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, _ := filepath.Rel(share, p)
		rel = filepath.ToSlash(rel)
		if d.IsDir() || rel == ManifestName {
			return nil
		}
		if d.Name() == ".DS_Store" && d.Type().IsRegular() {
			return nil // Finder writes one into any folder a user opens; it never holds anything stuntcam reads
		}
		if !d.Type().IsRegular() {
			return changed(rel + " is not a regular file")
		}
		want, ok := m.files[sharePre+rel]
		if !ok {
			return changed(rel + " is not in " + ManifestName)
		}
		have, err := sha256File(p)
		if err != nil {
			return err
		}
		if have != want {
			return changed(fmt.Sprintf("%s has sha256 %s, the manifest says %s", rel, have[:12], want[:12]))
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

// BundleSHA is a bundle's <sha> as helper/build.sh computes it: the first 12 hex digits of the sha1 over one
// "<path relative to the bundle> <sha1 hex>\n" line per file, in byte order of the paths.
func BundleSHA(files map[string][]byte) string {
	var rels []string
	for r := range files {
		rels = append(rels, r)
	}
	sort.Strings(rels)
	h := sha1.New()
	for _, r := range rels {
		fmt.Fprintf(h, "%s %x\n", r, sha1.Sum(files[r]))
	}
	return hex.EncodeToString(h.Sum(nil))[:12]
}

// HelperSHA is the <sha> directory the set's camera helper sits in, or "".
func (s Set) HelperSHA() string {
	if s.Helper == "" {
		return ""
	}
	return filepath.Base(filepath.Dir(s.Helper))
}

// unpack reads payload.tar (already checked against the manifest by checkShare), checks that its members are exactly the
// manifest's payload/ entries with those sha256s, and puts them under the host directory: dylibs at
// home.Builds()/<hash>/<name> (their dir names checked against their bytes, as ./build.sh names them), the canary at
// home.Canary()/<id>/m10canary.app. Copies already there are re-checked and rewritten if they differ.
func (m manifest) unpack(tarPath string) (unpacked, error) {
	var u unpacked
	want := map[string]string{}
	for p, h := range m.files {
		if strings.HasPrefix(p, payloadPre) {
			want[strings.TrimPrefix(p, payloadPre)] = h
		}
	}
	f, err := os.Open(tarPath)
	if err != nil {
		return u, err
	}
	defer f.Close()
	got := map[string]member{}
	tr := tar.NewReader(f)
	for {
		hd, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return u, changed(PayloadName + " is not a readable tar: " + err.Error())
		}
		name := strings.TrimPrefix(hd.Name, "./")
		if hd.Typeflag == tar.TypeDir {
			continue
		}
		if hd.Typeflag != tar.TypeReg || !safeRel(name) {
			return u, changed(fmt.Sprintf("%s has an unexpected member %q", PayloadName, hd.Name))
		}
		h, ok := want[name]
		if !ok {
			return u, changed(fmt.Sprintf("%s member %s is not in %s", PayloadName, name, ManifestName))
		}
		if _, dup := got[name]; dup {
			return u, changed(fmt.Sprintf("%s has %s twice", PayloadName, name))
		}
		b, err := io.ReadAll(io.LimitReader(tr, 64<<20))
		if err != nil {
			return u, err
		}
		if s := sha256.Sum256(b); hex.EncodeToString(s[:]) != h {
			return u, changed(fmt.Sprintf("%s member %s doesn't match %s", PayloadName, name, ManifestName))
		}
		got[name] = member{b, hd.Mode&0o111 != 0}
	}
	for name := range want {
		if _, ok := got[name]; !ok {
			return u, changed(fmt.Sprintf("%s has no %s", PayloadName, name))
		}
	}

	// the dylibs: group by dir, check each dir's name against its bytes, write to home.Builds()/<hash>/
	dirs := map[string][]string{} // "builds/<h>" -> member names
	var canary []string
	helpers := map[string][]string{} // <sha> -> member names
	for name := range got {
		if g := payloadDylib.FindStringSubmatch(name); g != nil {
			dirs[g[1]+"/"+g[2]] = append(dirs[g[1]+"/"+g[2]], name)
		} else if payloadCanary.MatchString(name) {
			canary = append(canary, name)
		} else if g := payloadHelper.FindStringSubmatch(name); g != nil {
			helpers[g[1]] = append(helpers[g[1]], name)
		} else {
			return u, changed(fmt.Sprintf("%s has an unexpected member %s", PayloadName, name))
		}
	}
	for dir, names := range dirs {
		kind, h := path.Dir(dir), path.Base(dir)
		var order []string // the hash order ./build.sh uses: engine, then loader
		switch kind {
		case "builds":
			order = []string{dir + "/" + EngineName, dir + "/" + LoaderName}
		case "stub":
			order = []string{dir + "/" + StubName}
		case "hook":
			order = []string{dir + "/" + HookName}
		}
		if len(names) != len(order) {
			return u, changed(fmt.Sprintf("%s: %s holds %v", PayloadName, dir, names))
		}
		hs := sha1.New()
		for _, n := range order {
			mb, ok := got[n]
			if !ok {
				return u, changed(fmt.Sprintf("%s has no %s", PayloadName, n))
			}
			hs.Write(mb.data)
		}
		if sum := hex.EncodeToString(hs.Sum(nil))[:12]; sum != h {
			return u, changed(fmt.Sprintf("%s hashes to %s, not %s", dir, sum, h))
		}
		for _, n := range order {
			dst := filepath.Join(home.Builds(), h, path.Base(n))
			if err := writeVerified(dst, got[n].data); err != nil {
				return u, err
			}
			switch path.Base(n) {
			case EngineName:
				if u.engine != "" {
					return u, changed(PayloadName + " has more than one engine")
				}
				u.engine = dst
			case LoaderName:
				u.loader = dst
			case StubName:
				if u.stub != "" {
					return u, changed(PayloadName + " has more than one stub")
				}
				u.stub = dst
			case HookName:
				if u.hook != "" {
					return u, changed(PayloadName + " has more than one runner hook")
				}
				u.hook = dst
			}
		}
	}
	if len(canary) > 0 {
		sort.Strings(canary)
		id := sha256.New()
		for _, n := range canary {
			fmt.Fprintf(id, "%s  %s\n", want[n], n)
		}
		files := map[string]member{}
		for _, n := range canary {
			files[strings.TrimPrefix(n, "canary/")] = got[n]
		}
		dir := filepath.Join(home.Canary(), hex.EncodeToString(id.Sum(nil))[:12])
		if err := unpackTree(dir, files, func(rel string) string { return want["canary/"+rel] }); err != nil {
			return u, err
		}
		u.canary = filepath.Join(dir, "m10canary.app")
	}
	if len(helpers) > 1 {
		return u, changed(PayloadName + " has more than one camera helper")
	}
	for sha, names := range helpers {
		// the dir name is the bundle's content hash; the executable keeps its exec bit (upload-artifact and npm drop
		// them elsewhere, a tar keeps them)
		files, data := map[string]member{}, map[string][]byte{}
		for _, n := range names {
			rel := strings.TrimPrefix(n, "helper/"+sha+"/")
			files[rel] = got[n]
			data[strings.TrimPrefix(rel, HelperApp+"/")] = got[n].data
		}
		if sum := BundleSHA(data); sum != sha {
			return u, changed(fmt.Sprintf("%s: helper/%s hashes to %s", PayloadName, sha, sum))
		}
		if exe, ok := files[HelperApp+"/"+HelperExe]; !ok || !exe.exec {
			return u, changed(fmt.Sprintf("%s: helper/%s has no executable %s", PayloadName, sha, HelperExe))
		}
		dir := filepath.Join(home.Root(), "helper", sha)
		if err := unpackTree(dir, files, func(rel string) string { return want["helper/"+sha+"/"+rel] }); err != nil {
			return u, err
		}
		u.helper = filepath.Join(dir, HelperApp)
	}
	return u, nil
}

// writeVerified makes dst hold data (0644, atomically), unless it already does.
func writeVerified(dst string, data []byte) error {
	if have, err := os.ReadFile(dst); err == nil && bytes.Equal(have, data) {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	if err := home.WriteAtomic(dst, data); err != nil {
		return err
	}
	if have, err := os.ReadFile(dst); err != nil || !bytes.Equal(have, data) {
		return fmt.Errorf("unpacked %s is not identical", dst)
	}
	return nil
}

// unpackTree makes dir hold exactly files (relative path -> bytes + exec bit): a dir that already does is kept (and its
// mtime set to now, which gc's "newest" goes by); else the tree is written to a temp dir that replaces it, so a canary
// install never reads half a bundle. Check, stage and swap run under an flock on <parent>/.lock, so two stuntcams
// unpacking at once (two commands right after an install) never rename away each other's dir: the second one waits,
// re-checks, and keeps the dir the first one wrote. A dir that matches is never renamed away.
func unpackTree(dir string, files map[string]member, sum func(rel string) string) error {
	if treeMatches(dir, files, sum) {
		now := time.Now()
		_ = os.Chtimes(dir, now, now)
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
	t, err := os.MkdirTemp(parent, ".stage-")
	if err != nil {
		return err
	}
	defer os.RemoveAll(t)
	if err := os.Chmod(t, 0o755); err != nil {
		return err
	}
	for rel, m := range files {
		p := filepath.Join(t, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			return err
		}
		mode := os.FileMode(0o644)
		if m.exec {
			mode = 0o755
		}
		if err := os.WriteFile(p, m.data, mode); err != nil {
			return err
		}
		if err := os.Chmod(p, mode); err != nil {
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
	return os.Rename(t, dir)
}

// LockDir takes an exclusive flock on dir/.lock (dir and the file are created if needed) and returns its release. The
// lock goes with the process: a stuntcam that dies holding it never blocks the next one.
func LockDir(dir string) (func(), error) {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(filepath.Join(dir, ".lock"), os.O_CREATE|os.O_RDONLY, 0o644) // flock works read-only; another user's .lock stays usable
	if err != nil {
		return nil, err
	}
	for {
		err = syscall.Flock(int(f.Fd()), syscall.LOCK_EX)
		if err != syscall.EINTR {
			break
		}
	}
	if err == syscall.ENOTSUP || err == syscall.EOPNOTSUPP || err == syscall.ENOLCK { // a filesystem without locks: go on
		f.Close() // go on unlocked
		return func() {}, nil
	}
	if err != nil {
		f.Close()
		return nil, fmt.Errorf("lock %s: %v", f.Name(), err)
	}
	return func() { syscall.Flock(int(f.Fd()), syscall.LOCK_UN); f.Close() }, nil
}

// treeMatches: dir is a directory holding exactly the regular files in files, each with sha256 sum(its relative path)
// and the exec bit files says (a chmod 644'd canary executable doesn't match, so it is unpacked again).
func treeMatches(dir string, files map[string]member, sum func(rel string) string) bool {
	if fi, err := os.Lstat(dir); err != nil || !fi.IsDir() {
		return false
	}
	count := 0
	bad := errors.New("differs")
	err := filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		rel, _ := filepath.Rel(dir, p)
		rel = filepath.ToSlash(rel)
		m, ok := files[rel]
		if !ok || !d.Type().IsRegular() {
			return bad
		}
		fi, err := d.Info()
		if err != nil || (fi.Mode().Perm()&0o111 != 0) != m.exec {
			return bad
		}
		if h, err := sha256File(p); err != nil || h != sum(rel) {
			return bad
		}
		count++
		return nil
	})
	return err == nil && count == len(files)
}
