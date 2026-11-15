// SPDX-License-Identifier: Apache-2.0

// Package artifacts finds the dylibs, profiles and media stuntcam arms a simulator with, and stages them under the host
// directory (home.Builds) where simulator processes can read them.
//
// A build directory holds a matching libsimcamloader.dylib + libsimcam.dylib pair; the frozen stub lives in its own directory.
// Copies are byte-identical, so the ad-hoc linker signature stays valid (never re-sign: design, Mac rules).
package artifacts

import (
	"bytes"
	"crypto/sha1"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

const (
	StubName   = "libsimcamstub.dylib"
	LoaderName = "libsimcamloader.dylib"
	EngineName = "libsimcam.dylib"
	HookName   = "libsimcamrunnerhook.dylib" // the XCTest runner hook `exec` inserts (design §4.2); optional: older builds lack it
)

// Set is one resolved build: absolute paths of the three dylibs, the default profile and the scene's fixture image.
type Set struct {
	Stub, Loader, Engine string
	Hook                 string // the runner hook ("" if this build has none: exec then warns instead of hooking)
	Profile, Image       string
	Canary               string // the canary .app (verify); optional for arming
	Rules                string // a restore-rules/ dir (<xcode build>.json files); after Stage, its per-content copy ("" if none)
	Verdicts             string // shipped verdicts (<key>.json canary passes from the release gate); installed layout only, else ""
	From                 string // where it was found (for status/doctor)
	Installed            bool   // found in the installed layout (npm package, Homebrew keg) rather than a checkout or --build
	Version              string // the installed package's "<version> (commit <c>)" from its manifest ("" otherwise)
}

// Find resolves a build: dir (a directory with the loader + engine pair, or ""), else $STUNTCAM_BUILD (with $STUNTCAM_STUB,
// $STUNTCAM_PROFILE, $STUNTCAM_IMAGE, $STUNTCAM_CANARY overriding what is found around it), else a stuntcam
// checkout found from the working directory upwards (its build/current_*.txt, as ./build.sh writes them), else the checkout
// the executable was built in (cli/bin/stuntcam on PATH, run from an app's directory), else the installed layout next to
// the executable (Installed), which the npm package and the Homebrew formula use.
//
// An installed stuntcam (bin/stuntcam next to share/stuntcam/manifest.txt) uses its own package unless told otherwise:
//   - --build / $STUNTCAM_BUILD: that build; the canary, profile, image and restore rules come from around it, else from
//     the installed package (a staged $SIMCAM_HOME/builds/<hash> has none of its own);
//   - $STUNTCAM_USE_CHECKOUT=1: the working directory's checkout, only if trustedCheckout passes;
//   - never the checkout the executable sits in (node_modules is inside the user's project, not a stuntcam checkout).
//
// A stuntcam checkout around the working directory without the opt-in is ignored with a note, and every build that isn't
// the package's own is announced (Note) with its build, canary, profile and rules.
func Find(dir string) (Set, error) {
	wd, _ := os.Getwd()
	return find(dir, installedPrefix(), wd)
}

// UseCheckoutEnv opts an installed stuntcam into the working directory's checkout.
const UseCheckoutEnv = "STUNTCAM_USE_CHECKOUT"

// find is Find with the installed prefix ("" when the executable isn't installed) and the working directory given.
func find(dir, prefix, wd string) (Set, error) {
	if dir == "" {
		dir = os.Getenv("STUNTCAM_BUILD")
	}
	if dir != "" {
		var pkg *Set
		if prefix != "" {
			if p, err := Installed(prefix); err == nil {
				pkg = &p
			}
		}
		s, err := fromDir(dir, pkg, prefix != "")
		if err == nil && prefix != "" {
			note(fmt.Sprintf("using the build in %s (--build or STUNTCAM_BUILD), not the installed package's: %s", s.From, s.Describe()))
		}
		return s, err
	}
	if prefix != "" {
		if s, err := checkoutFrom(wd); !errors.Is(err, errNoCheckout) {
			switch {
			case os.Getenv(UseCheckoutEnv) != "1":
				if stuntcamRoot(s.From) == nil {
					note(fmt.Sprintf("ignoring the stuntcam checkout in %s (an installed stuntcam uses a checkout's build only with %s=1 or --build <dir>); using the installed package's build", s.From, UseCheckoutEnv))
				}
			default:
				if terr := trustedCheckout(s, err); terr != nil {
					note(fmt.Sprintf("ignoring the build pointers in %s: %v; using the installed package's build", filepath.Join(s.From, "build"), terr))
				} else {
					note(fmt.Sprintf("using the stuntcam checkout in %s (%s=1), not the installed package: %s", s.From, UseCheckoutEnv, s.Describe()))
					return s, nil
				}
			}
		}
		return Installed(prefix)
	}
	if s, err := checkoutFrom(wd); err == nil {
		return s, nil
	} else if !errors.Is(err, errNoCheckout) {
		return s, err // in a checkout whose build is missing or stale: say so, never fall back to another checkout
	}
	if s, err := exeCheckout(); err == nil {
		return s, nil
	} else if !errors.Is(err, errNoCheckout) {
		return s, err
	}
	return Set{}, errors.New("no stuntcam build found: run ./build.sh in a stuntcam checkout, pass --build <dir>, or set STUNTCAM_BUILD")
}

// InstalledHere is the installed package the executable belongs to (Installed of its prefix), whatever the working
// directory, --build or $STUNTCAM_BUILD say: what `stuntcam version --check` and the formula's test check.
func InstalledHere() (Set, error) {
	prefix := installedPrefix()
	if prefix == "" {
		exe, _ := os.Executable()
		return Set{}, fmt.Errorf("not an installed stuntcam: no share/stuntcam/%s next to %s (a dev build: `stuntcam doctor` shows its build)", ManifestName, exe)
	}
	return Installed(prefix)
}

// Describe names what a set arms and verifies with: the build ids and the canary, profile and restore rules paths.
func (s Set) Describe() string {
	or := func(p string) string {
		if p == "" {
			return "none"
		}
		return p
	}
	return fmt.Sprintf("engine+loader %s, stub %s, canary %s, profile %s, restore rules %s",
		s.BuildID(), filepath.Base(filepath.Dir(s.Stub)), or(s.Canary), or(s.Profile), or(s.Rules))
}

// Note prints a one-line note about which build Find uses (stderr, so --json output stays clean); each text once per process.
var Note = func(msg string) { fmt.Fprintln(os.Stderr, "stuntcam: "+msg) }

var (
	noteMu   sync.Mutex
	noteSeen = map[string]bool{}
)

func note(msg string) {
	noteMu.Lock()
	seen := noteSeen[msg]
	noteSeen[msg] = true
	noteMu.Unlock()
	if !seen {
		Note(msg)
	}
}

// installedPrefix is the prefix of the installed layout the executable runs from (the dir holding bin/stuntcam and
// share/stuntcam/manifest.txt), else "". npm runs the binary through node_modules/.bin/stuntcam and Homebrew through
// $(brew --prefix)/bin/stuntcam, both symlinks: the layout is next to the real file.
func installedPrefix() string {
	exe, err := os.Executable()
	if err != nil {
		return ""
	}
	if r, err := filepath.EvalSymlinks(exe); err == nil {
		exe = r
	}
	p := filepath.Dir(filepath.Dir(exe))
	if exists(filepath.Join(p, "share", "stuntcam", ManifestName)) {
		return p
	}
	return ""
}

// trustedCheckout says why an installed stuntcam must not use checkout s (checkoutFrom's result and error), nil if it may:
// s.From must be a stuntcam root (build.sh + engine/src/simcam.m), and its build pointers must name files under
// home.Builds()/<12 hex>/ whose bytes hash to that name (as ./build.sh stages them).
func trustedCheckout(s Set, cerr error) error {
	if err := stuntcamRoot(s.From); err != nil {
		return err
	}
	if cerr != nil {
		return cerr
	}
	builds, err := filepath.EvalSymlinks(home.Builds())
	if err != nil {
		return fmt.Errorf("they don't point into %s", home.Builds())
	}
	if b, err := os.ReadFile(filepath.Join(s.From, "build", "current_dylib.txt")); err == nil && strings.TrimSpace(string(b)) != s.Engine {
		return fmt.Errorf("current_dylib.txt and current_loader.txt name different builds")
	}
	type want struct {
		files []string
		name  string
	}
	ws := []want{{[]string{s.Engine, s.Loader}, "engine+loader"}, {[]string{s.Stub}, "stub"}}
	if s.Hook != "" {
		ws = append(ws, want{[]string{s.Hook}, "runner hook"})
	}
	for _, w := range ws {
		dir := ""
		for _, f := range w.files {
			if !filepath.IsAbs(f) {
				return fmt.Errorf("the %s pointer %q is not an absolute path", w.name, f)
			}
			r, err := filepath.EvalSymlinks(f)
			if err != nil {
				return fmt.Errorf("the %s pointer names a missing file %s", w.name, f)
			}
			d := filepath.Dir(r)
			if filepath.Dir(d) != builds || !hexDir.MatchString(filepath.Base(d)) {
				return fmt.Errorf("the %s pointer %s is not in a content-hash dir under %s", w.name, f, home.Builds())
			}
			if dir != "" && d != dir {
				return fmt.Errorf("the engine and loader are in different dirs")
			}
			dir = d
		}
		h, err := hashFiles(w.files...)
		if err != nil {
			return err
		}
		if h != filepath.Base(dir) {
			return fmt.Errorf("%s hashes to %s, not its dir name %s", w.name, h, filepath.Base(dir))
		}
	}
	return nil
}

// stuntcamRoot: d is a stuntcam checkout's root (build.sh and engine/src/simcam.m are regular files).
func stuntcamRoot(d string) error {
	for _, f := range []string{"build.sh", filepath.Join("engine", "src", "simcam.m")} {
		if fi, err := os.Stat(filepath.Join(d, f)); err != nil || !fi.Mode().IsRegular() {
			return fmt.Errorf("not a stuntcam checkout (no %s)", f)
		}
	}
	return nil
}

// fromDir resolves a --build dir. pkg (the installed package, or nil) supplies the canary, profile, image and rules a
// staged build dir has none of; installed: the binary is installed, so no checkout (cwd or the executable's) is consulted.
func fromDir(dir string, pkg *Set, installed bool) (Set, error) {
	dir, _ = filepath.Abs(dir)
	s := Set{Loader: filepath.Join(dir, LoaderName), Engine: filepath.Join(dir, EngineName), From: dir}
	stub := os.Getenv("STUNTCAM_STUB")
	if stub == "" {
		stub = newest(filepath.Join(filepath.Dir(dir), "*", StubName))
	}
	s.Stub = stub
	s.Hook = os.Getenv("STUNTCAM_HOOK")
	if s.Hook == "" {
		s.Hook = newest(filepath.Join(filepath.Dir(dir), "*", HookName))
	}
	// a staged build ($SIMCAM_HOME/builds/<hash>) has its profile and image in $SIMCAM_HOME/dev; a release or checkout layout
	// has them two levels up
	s.Profile, s.Image = dataFiles(filepath.Dir(filepath.Dir(dir)))
	if p := os.Getenv("STUNTCAM_PROFILE"); p != "" {
		s.Profile = p
	}
	if p := os.Getenv("STUNTCAM_IMAGE"); p != "" {
		s.Image = p
	}
	if pkg != nil {
		if s.Profile == "" {
			s.Profile = pkg.Profile
		}
		if s.Image == "" {
			s.Image = pkg.Image
		}
	}
	if s.Profile == "" || s.Image == "" {
		p, i := dataFiles(home.Dev())
		if s.Profile == "" {
			s.Profile = p
		}
		if s.Image == "" {
			s.Image = i
		}
	}
	// the canary: $STUNTCAM_CANARY, else the release layout's (share/stuntcam/canary next to builds/), else the installed
	// package's (an installed binary), else a checkout's whose current build is this one (another checkout's canary may
	// check things this engine predates)
	if c := os.Getenv("STUNTCAM_CANARY"); c != "" {
		s.Canary = c
	} else if c := filepath.Join(filepath.Dir(filepath.Dir(dir)), "canary", "m10canary.app"); !inHome(dir) && exists(c) {
		s.Canary = c
	} else if pkg != nil {
		s.Canary = pkg.Canary
	} else if !installed {
		for _, co := range checkouts() {
			if filepath.Base(filepath.Dir(co.Loader)) == filepath.Base(dir) {
				s.Canary = co.Canary
				break
			}
		}
	}
	s.Rules = os.Getenv("STUNTCAM_RULES")
	if s.Rules == "" {
		// a staged build ($SIMCAM_HOME/builds/<h>): $SIMCAM_HOME/restore-rules is the staging cache (only <hash>/ dirs), not
		// a rules dir — take the package's or a checkout's
		if r := filepath.Join(filepath.Dir(filepath.Dir(dir)), "restore-rules"); !inHome(dir) && exists(r) {
			s.Rules = r
		} else if pkg != nil {
			s.Rules = pkg.Rules
		} else if !installed {
			for _, co := range checkouts() {
				s.Rules = co.Rules
				break
			}
		}
	}
	if err := s.check(); err != nil {
		return s, err
	}
	// without M2_PLIST the engine throws in m2_newFigCaptureSources and every app crashes at device discovery
	if s.Profile == "" || !exists(s.Profile) {
		return s, fmt.Errorf("no camera profile for the build in %s: expected profiles/cfg-m10.plist two levels up or in $SIMCAM_HOME/dev; set STUNTCAM_PROFILE", dir)
	}
	return s, nil
}

func fromCheckout() (Set, error) {
	wd, err := os.Getwd()
	if err != nil {
		return Set{}, err
	}
	return checkoutFrom(wd)
}

var errNoCheckout = errors.New("not in a stuntcam checkout")

func exeCheckout() (Set, error) {
	exe, err := os.Executable()
	if err == nil {
		exe, err = filepath.EvalSymlinks(exe)
	}
	if err != nil {
		return Set{}, errNoCheckout
	}
	return checkoutFrom(filepath.Dir(exe))
}

// checkoutFrom finds a stuntcam checkout (build/current_loader.txt) from start upwards: errNoCheckout if there is none,
// another error if there is one whose build is missing.
func checkoutFrom(start string) (Set, error) {
	for d := start; ; d = filepath.Dir(d) {
		if b, err := os.ReadFile(filepath.Join(d, "build", "current_loader.txt")); err == nil {
			st, _ := os.ReadFile(filepath.Join(d, "build", "current_stub.txt"))
			hk, _ := os.ReadFile(filepath.Join(d, "build", "current_hook.txt"))
			ldr := strings.TrimSpace(string(b))
			s := Set{Stub: strings.TrimSpace(string(st)), Hook: strings.TrimSpace(string(hk)), Loader: ldr, Engine: filepath.Join(filepath.Dir(ldr), EngineName),
				Profile: filepath.Join(d, "engine", "profiles", "cfg-m10.plist"), Image: filepath.Join(d, "fixtures", "media", "qr.png"),
				Canary: filepath.Join(d, "canary", "build", "m10canary.app"), Rules: filepath.Join(d, "restore-rules"), From: d}
			return s, s.check()
		}
		if filepath.Dir(d) == d {
			return Set{}, errNoCheckout
		}
	}
}

// dataFiles looks for profiles/cfg-m10.plist and media/qr.png under root, or the checkout layout (engine/profiles, fixtures/media).
func dataFiles(root string) (profile, image string) {
	for _, p := range []string{filepath.Join(root, "profiles", "cfg-m10.plist"), filepath.Join(root, "engine", "profiles", "cfg-m10.plist")} {
		if exists(p) {
			profile = p
			break
		}
	}
	for _, p := range []string{filepath.Join(root, "media", "qr.png"), filepath.Join(root, "fixtures", "media", "qr.png")} {
		if exists(p) {
			image = p
			break
		}
	}
	return
}

func newest(glob string) string {
	m, _ := filepath.Glob(glob)
	var best string
	var bt int64
	for _, p := range m {
		if fi, err := os.Stat(p); err == nil && fi.ModTime().UnixNano() > bt {
			best, bt = p, fi.ModTime().UnixNano()
		}
	}
	return best
}

func exists(p string) bool { _, err := os.Stat(p); return err == nil }

func (s Set) check() error {
	for _, p := range []string{s.Stub, s.Loader, s.Engine} {
		if p == "" || !exists(p) {
			return fmt.Errorf("missing build product %q (found under %s)", p, s.From)
		}
	}
	return nil
}

var hexDir = regexp.MustCompile(`^[0-9a-f]{12}$`)

// Stage copies the set under home.Builds()/<hash>/ (unless it is already there) and the profile + image under
// home.Data()/<hash>/, so staging another profile never changes what an armed sim (named by path) reads.
// The pair dir keeps its name when it already is a 12-hex content hash; otherwise the name is the sha1 of engine+loader
// bytes, as ./build.sh computes it (`cat libsimcam.dylib libsimcamloader.dylib | shasum`). Returns the staged set.
func Stage(s Set) (Set, error) {
	out := s
	var err error
	pairHash := filepath.Base(filepath.Dir(s.Loader))
	if !hexDir.MatchString(pairHash) {
		if pairHash, err = hashFiles(s.Engine, s.Loader); err != nil {
			return s, err
		}
	}
	stubHash := filepath.Base(filepath.Dir(s.Stub))
	if !hexDir.MatchString(stubHash) {
		if stubHash, err = hashFiles(s.Stub); err != nil {
			return s, err
		}
	}
	if out.Loader, err = copyInto(s.Loader, filepath.Join(home.Builds(), pairHash)); err != nil {
		return s, err
	}
	if out.Engine, err = copyInto(s.Engine, filepath.Join(home.Builds(), pairHash)); err != nil {
		return s, err
	}
	if out.Stub, err = copyInto(s.Stub, filepath.Join(home.Builds(), stubHash)); err != nil {
		return s, err
	}
	if s.Hook != "" && exists(s.Hook) { // its own content-hash dir, like the stub
		hookHash := filepath.Base(filepath.Dir(s.Hook))
		if !hexDir.MatchString(hookHash) {
			if hookHash, err = hashFiles(s.Hook); err != nil {
				return s, err
			}
		}
		if out.Hook, err = copyInto(s.Hook, filepath.Join(home.Builds(), hookHash)); err != nil {
			return s, err
		}
	} else {
		out.Hook = ""
	}
	if out.Profile, err = StageData(s.Profile); err != nil {
		return s, err
	}
	if out.Image, err = StageData(s.Image); err != nil {
		return s, err
	}
	if out.Rules, err = StageRules(s.Rules); err != nil {
		return s, err
	}
	return out, nil
}

// RulesHash is the content hash of a restore-rules dir: sha1 over one "<file name> <sha1 hex of its bytes>\n" line per
// [0-9]*.json file in byte order, first 12 hex digits; and those files. scripts/env.sh computes the same hash for the same
// files (`print -r -- "${f:t} $(shasum < $f | cut -c1-40)" | shasum | cut -c1-12`, LC_ALL=C glob order).
func RulesHash(dir string) (string, []string, error) {
	m, err := filepath.Glob(filepath.Join(dir, "[0-9]*.json")) // sorted, bytewise
	if err != nil {
		return "", nil, err
	}
	var files []string
	h := sha1.New()
	for _, f := range m {
		if st, err := os.Stat(f); err != nil || !st.Mode().IsRegular() {
			continue
		}
		b, err := os.ReadFile(f)
		if err != nil {
			return "", nil, err
		}
		fmt.Fprintf(h, "%s %x\n", filepath.Base(f), sha1.Sum(b))
		files = append(files, f)
	}
	return hex.EncodeToString(h.Sum(nil))[:12], files, nil
}

// StageRules copies dir's <build>.json files to home.Rules()/<content hash>/ (RulesHash) and returns that dir, which the
// sim's config.json names as rules_dir: a sim armed with one set of rules never sees another clone's or a later edit's
// files, and no stale file feeds the loader's "newest" best guess (as scripts/env.sh stages dev/<hash>/).
// Staged in a temp dir that is renamed once complete (with a .complete marker), so a launch never reads half a copy.
// No dir, or no rules in it: "".
func StageRules(dir string) (string, error) {
	if dir == "" {
		return "", nil
	}
	h, files, err := RulesHash(dir)
	if err != nil || len(files) == 0 {
		return "", err
	}
	dst := filepath.Join(home.Rules(), h)
	if _, err := os.Stat(filepath.Join(dst, ".complete")); err == nil {
		return dst, nil
	}
	if err := os.MkdirAll(home.Rules(), 0o755); err != nil {
		return "", err
	}
	t, err := os.MkdirTemp(home.Rules(), ".stage-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(t) // gone after a successful rename; a failed stage leaves nothing
	if err := os.Chmod(t, 0o755); err != nil {
		return "", err
	}
	for _, f := range files {
		if _, err := copyInto(f, t); err != nil {
			return "", err
		}
	}
	if err := os.WriteFile(filepath.Join(t, ".complete"), nil, 0o644); err != nil {
		return "", err
	}
	if err := os.Rename(t, dst); err != nil {
		if _, e := os.Stat(filepath.Join(dst, ".complete")); e == nil {
			return dst, nil // staged by another stuntcam or script meanwhile: the same content
		}
		return "", err
	}
	return dst, nil
}

// StageData copies a profile, image or video to home.Data()/<sha1-12 of its bytes>/<name> ("" stays ""), where simulator
// processes can read it and a later file with the same name never replaces what an armed sim reads.
func StageData(src string) (string, error) {
	if src == "" {
		return "", nil
	}
	h, err := hashFiles(src)
	if err != nil {
		return "", err
	}
	return copyInto(src, filepath.Join(home.Data(), h))
}

// BuildID is the name of the loader+engine dir: what the sentinel records as "build" and verdict keys use.
func (s Set) BuildID() string { return filepath.Base(filepath.Dir(s.Loader)) }

func hashFiles(paths ...string) (string, error) {
	h := sha1.New()
	for _, p := range paths {
		f, err := os.Open(p)
		if err != nil {
			return "", err
		}
		_, err = io.Copy(h, f)
		f.Close()
		if err != nil {
			return "", err
		}
	}
	return hex.EncodeToString(h.Sum(nil))[:12], nil
}

// copyInto copies src into dir (same base name) unless an identical file is already there, and verifies the copy.
func copyInto(src, dir string) (string, error) {
	dst := filepath.Join(dir, filepath.Base(src))
	if a, _ := filepath.Abs(src); a == dst {
		return dst, nil
	}
	want, err := os.ReadFile(src)
	if err != nil {
		return "", err
	}
	if have, err := os.ReadFile(dst); err == nil && bytes.Equal(have, want) {
		return dst, nil
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return "", err
	}
	if err := home.WriteAtomic(dst, want); err != nil {
		return "", err
	}
	if have, err := os.ReadFile(dst); err != nil || !bytes.Equal(have, want) {
		return "", fmt.Errorf("copy of %s is not identical", src)
	}
	return dst, nil
}

// checkouts: the checkout around the working directory and the one the executable was built in, where found and built
// (a binary that isn't installed only: fromDir never asks for an installed one).
func checkouts() []Set {
	var out []Set
	if s, err := fromCheckout(); err == nil {
		out = append(out, s)
	}
	if s, err := exeCheckout(); err == nil {
		out = append(out, s)
	}
	return out
}

// inHome: dir is a build staged under the stuntcam home ($SIMCAM_HOME/builds/<h>), whose neighbours are caches, not a
// checkout's canary/ and restore-rules/.
func inHome(dir string) bool {
	return filepath.Clean(filepath.Dir(filepath.Dir(dir))) == filepath.Clean(home.Root())
}
