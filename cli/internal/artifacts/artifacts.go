// SPDX-License-Identifier: Apache-2.0

// Package artifacts finds the dylibs, profiles and media stuntcam arms a simulator with, and stages them under the host
// directory (home.Builds) where simulator processes can read them.
//
// A build directory holds a matching pair: libsimcamloader.dylib and libsimcam.dylib. The frozen stub lives in a
// directory of its own.
// Copies are byte-identical, so the ad-hoc linker signature stays valid. Never re-sign them.
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
	HookName   = "libsimcamrunnerhook.dylib" // the XCTest runner hook `exec` inserts; optional: older builds lack it
)

// Set is one resolved build: absolute paths of the three dylibs, the default profile and the scene's fixture image.
type Set struct {
	Stub, Loader, Engine string
	Hook                 string // the runner hook ("" if this build has none: exec then warns instead of hooking)
	Profile, Image       string
	Canary               string // the canary .app (verify); optional for arming
	Helper               string // the camera helper, $SIMCAM_HOME/helper/<sha>/Stuntcam Camera.app ("" if this build has none)
	Rules                string // a restore-rules/ dir (<xcode build>.json files); after Stage, its per-content copy ("" if none)
	Verdicts             string // shipped verdicts (<key>.json canary passes from the release gate); installed layout only, else ""
	From                 string // where it was found (for status/doctor)
	Installed            bool   // found in the installed layout (npm package, Homebrew keg) rather than a checkout or --build
	Version              string // the installed package's "<version> (commit <c>)" from its manifest ("" otherwise)
}

// Find resolves a build from the first of these that applies:
//  1. dir, a directory with the loader and engine pair ("" for none);
//  2. $STUNTCAM_BUILD. $STUNTCAM_STUB, $STUNTCAM_PROFILE, $STUNTCAM_IMAGE and $STUNTCAM_CANARY override what is found
//     around it;
//  3. a stuntcam checkout found from the working directory upwards, through the build/current_*.txt files that
//     ./build.sh writes;
//  4. the checkout the executable was built in, as when cli/bin/stuntcam is on PATH and runs from an app's directory;
//  5. the installed layout next to the executable (Installed), which the npm package and the Homebrew formula use.
//
// An installed stuntcam (bin/stuntcam next to share/stuntcam/manifest.txt) uses its own package unless told otherwise:
//   - --build or $STUNTCAM_BUILD picks that build. The canary, profile, image and restore rules come from around it,
//     or else from the installed package, because a staged $SIMCAM_HOME/builds/<hash> has none of its own.
//   - $STUNTCAM_USE_CHECKOUT=1 picks the working directory's checkout, but only if trustedCheckout passes.
//   - It never uses the checkout the executable sits in. node_modules is inside the user's project, not a stuntcam
//     checkout.
//
// Without the opt-in, a stuntcam checkout around the working directory is ignored with a note. Every build that isn't
// the package's own is announced (Note) with its build, canary, profile and rules.
func Find(dir string) (Set, error) {
	wd, _ := os.Getwd() // "" if the working directory is gone: no checkout from it
	return find(dir, installedPrefix(), wd)
}

// UseCheckoutEnv opts an installed stuntcam into the working directory's checkout.
const UseCheckoutEnv = "STUNTCAM_USE_CHECKOUT"

// find is Find with the installed prefix ("" when the executable isn't installed) and the working directory given.
func find(dir, prefix, wd string) (Set, error) {
	if dir == "" {
		dir = os.Getenv("STUNTCAM_BUILD")
	}

	switch {
	case dir != "":
		return fromBuildDir(dir, prefix)
	case prefix != "":
		return installedBuild(prefix, wd)
	}

	return devBuild(wd)
}

// fromBuildDir is the build in dir (--build or $STUNTCAM_BUILD). An installed stuntcam fills in what dir lacks from
// its package and says that it isn't using the package's build.
func fromBuildDir(dir, prefix string) (Set, error) {
	var pkg *Set
	if prefix != "" {
		if p, err := Installed(prefix); err == nil {
			pkg = &p
		}
	}

	set, err := fromDir(dir, pkg, prefix != "")
	if err == nil && prefix != "" {
		note(fmt.Sprintf("using the build in %s (--build or STUNTCAM_BUILD), not the installed package's: %s", set.From,
			set.Describe()))
	}

	return set, err
}

// installedBuild is an installed stuntcam's build: its package's, unless $STUNTCAM_USE_CHECKOUT=1 and the working
// directory's checkout passes trustedCheckout. A checkout it doesn't use gets a note.
func installedBuild(prefix, wd string) (Set, error) {
	set, err := checkoutFrom(wd)
	if errors.Is(err, errNoCheckout) {
		return Installed(prefix)
	}

	if os.Getenv(UseCheckoutEnv) != "1" {
		if stuntcamRoot(set.From) == nil {
			note(fmt.Sprintf("ignoring the stuntcam checkout in %s (an installed stuntcam uses a checkout's build "+
				"only with %s=1 or --build <dir>); using the installed package's build", set.From, UseCheckoutEnv))
		}

		return Installed(prefix)
	}

	if terr := trustedCheckout(set, err); terr != nil {
		pointers := filepath.Join(set.From, "build")
		note(fmt.Sprintf("ignoring the build pointers in %s: %v; using the installed package's build", pointers, terr))
		return Installed(prefix)
	}

	note(fmt.Sprintf("using the stuntcam checkout in %s (%s=1), not the installed package: %s",
		set.From, UseCheckoutEnv, set.Describe()))
	return set, nil
}

// devBuild is a dev build's: the working directory's checkout, else the checkout the executable was built in.
func devBuild(wd string) (Set, error) {
	set, err := checkoutFrom(wd)
	if err == nil {
		return set, nil
	}

	if !errors.Is(err, errNoCheckout) {
		return set, err // in a checkout whose build is missing or stale: say so, never fall back to another checkout
	}

	set, err = exeCheckout()
	if err == nil {
		return set, nil
	}

	if !errors.Is(err, errNoCheckout) {
		return set, err
	}

	return Set{}, errors.New("no stuntcam build found: run ./build.sh in a stuntcam checkout, pass --build <dir>, or " +
		"set STUNTCAM_BUILD")
}

// InstalledHere is the installed package the executable belongs to (Installed of its prefix), whatever the working
// directory, --build or $STUNTCAM_BUILD say: what `stuntcam version --check` and the formula's test check.
func InstalledHere() (Set, error) {
	prefix := installedPrefix()
	if prefix == "" {
		exe, _ := os.Executable() // only for the message
		return Set{}, fmt.Errorf("not an installed stuntcam: no share/stuntcam/%s next to %s "+
			"(a dev build: `stuntcam doctor` shows its build)", ManifestName, exe)
	}
	return Installed(prefix)
}

// Describe names what a set arms and verifies with: the build ids and the canary, profile and restore rules paths.
func (s Set) Describe() string {
	orNone := func(p string) string {
		if p == "" {
			return "none"
		}
		return p
	}
	return fmt.Sprintf("engine+loader %s, stub %s, canary %s, profile %s, restore rules %s",
		s.BuildID(), filepath.Base(filepath.Dir(s.Stub)), orNone(s.Canary), orNone(s.Profile), orNone(s.Rules))
}

// Note prints a one-line note about which build Find uses (stderr, so --json output stays clean); each text once per
// process.
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

// trustedCheckout says why an installed stuntcam must not use the checkout set, or returns nil if it may. set and cerr
// are checkoutFrom's result and error.
// set.From must be a stuntcam root, with build.sh and engine/src/simcam.m. Its build pointers must name files under
// home.Builds()/<12 hex>/ whose bytes hash to that name, as ./build.sh stages them.
func trustedCheckout(set Set, cerr error) error {
	if err := stuntcamRoot(set.From); err != nil {
		return err
	}

	if cerr != nil {
		return cerr
	}

	builds, err := filepath.EvalSymlinks(home.Builds())
	if err != nil {
		return fmt.Errorf("they don't point into %s", home.Builds())
	}

	dylibPointer, err := os.ReadFile(filepath.Join(set.From, "build", "current_dylib.txt"))
	if err == nil && strings.TrimSpace(string(dylibPointer)) != set.Engine {
		return fmt.Errorf("current_dylib.txt and current_loader.txt name different builds")
	}

	type want struct {
		files []string
		name  string
	}

	groups := []want{{[]string{set.Engine, set.Loader}, "engine+loader"}, {[]string{set.Stub}, "stub"}}
	if set.Hook != "" {
		groups = append(groups, want{[]string{set.Hook}, "runner hook"})
	}

	for _, group := range groups {
		dir := ""
		for _, pointer := range group.files {
			if !filepath.IsAbs(pointer) {
				return fmt.Errorf("the %s pointer %q is not an absolute path", group.name, pointer)
			}

			resolved, err := filepath.EvalSymlinks(pointer)
			if err != nil {
				return fmt.Errorf("the %s pointer names a missing file %s", group.name, pointer)
			}

			pointerDir := filepath.Dir(resolved)
			if filepath.Dir(pointerDir) != builds || !hexDir.MatchString(filepath.Base(pointerDir)) {
				return fmt.Errorf("the %s pointer %s is not in a content-hash dir under %s", group.name, pointer,
					home.Builds())
			}

			if dir != "" && pointerDir != dir {
				return fmt.Errorf("the engine and loader are in different dirs")
			}

			dir = pointerDir
		}

		sum, err := hashFiles(group.files...)
		if err != nil {
			return err
		}

		if sum != filepath.Base(dir) {
			return fmt.Errorf("%s hashes to %s, not its dir name %s", group.name, sum, filepath.Base(dir))
		}
	}

	return nil
}

// stuntcamRoot returns nil if d is a stuntcam checkout's root (build.sh and engine/src/simcam.m are regular files), else
// what is missing.
func stuntcamRoot(d string) error {
	for _, f := range []string{"build.sh", filepath.Join("engine", "src", "simcam.m")} {
		if fi, err := os.Stat(filepath.Join(d, f)); err != nil || !fi.Mode().IsRegular() {
			return fmt.Errorf("not a stuntcam checkout (no %s)", f)
		}
	}
	return nil
}

// fromDir resolves a --build dir.
// pkg is the installed package, or nil. It supplies the canary, profile, image and rules that a staged build dir has
// none of.
// installed says the binary is installed. Then no checkout is consulted, neither the working directory's nor the
// executable's.
func fromDir(dir string, pkg *Set, installed bool) (Set, error) {
	dir, _ = filepath.Abs(dir) // fails only if the working directory is gone
	set := Set{Loader: filepath.Join(dir, LoaderName), Engine: filepath.Join(dir, EngineName), From: dir}
	if set.Stub = os.Getenv("STUNTCAM_STUB"); set.Stub == "" {
		set.Stub = newest(filepath.Join(filepath.Dir(dir), "*", StubName))
	}

	if set.Hook = os.Getenv("STUNTCAM_HOOK"); set.Hook == "" {
		set.Hook = newest(filepath.Join(filepath.Dir(dir), "*", HookName))
	}

	set.Profile, set.Image = dataFor(dir, pkg)
	set.Canary = canaryFor(dir, pkg, installed)
	set.Helper = helperFor(pkg, installed)
	set.Rules = rulesFor(dir, pkg, installed)
	if err := set.check(); err != nil {
		return set, err
	}

	// without SIMCAM_PROFILE the engine throws in hook_videoNewFigCaptureSources and every app crashes at device
	// discovery
	if set.Profile == "" || !exists(set.Profile) {
		return set, fmt.Errorf("no camera profile for the build in %s: expected profiles/cfg-m10.plist two levels up "+
			"or in $SIMCAM_HOME/dev; set STUNTCAM_PROFILE", dir)
	}

	return set, nil
}

// dataFor finds a build dir's profile and image: $STUNTCAM_PROFILE / $STUNTCAM_IMAGE, else the ones two levels up (a
// release or checkout layout), else the package's, else $SIMCAM_HOME/dev's (where a staged build's are).
func dataFor(dir string, pkg *Set) (profile, image string) {
	profile, image = dataFiles(filepath.Dir(filepath.Dir(dir)))
	if p := os.Getenv("STUNTCAM_PROFILE"); p != "" {
		profile = p
	}

	if p := os.Getenv("STUNTCAM_IMAGE"); p != "" {
		image = p
	}

	if pkg != nil {
		if profile == "" {
			profile = pkg.Profile
		}

		if image == "" {
			image = pkg.Image
		}
	}

	if profile == "" || image == "" {
		p, i := dataFiles(home.Dev())
		if profile == "" {
			profile = p
		}

		if image == "" {
			image = i
		}
	}

	return profile, image
}

// canaryFor finds a build dir's canary, from the first of these that has one:
//  1. $STUNTCAM_CANARY;
//  2. the release layout, with share/stuntcam/canary next to builds/;
//  3. the installed package, for an installed binary;
//  4. a checkout whose current build is this one. Another checkout's canary may check things this engine predates.
func canaryFor(dir string, pkg *Set, installed bool) string {
	if c := os.Getenv("STUNTCAM_CANARY"); c != "" {
		return c
	}

	if c := filepath.Join(filepath.Dir(filepath.Dir(dir)), "canary", "m10canary.app"); !inHome(dir) && exists(c) {
		return c
	}

	if pkg != nil {
		return pkg.Canary
	}

	if !installed {
		for _, checkout := range checkouts() {
			if filepath.Base(filepath.Dir(checkout.Loader)) == filepath.Base(dir) {
				return checkout.Canary
			}
		}
	}

	return ""
}

// helperFor finds the camera helper for a --build dir: $STUNTCAM_HELPER, else the installed package's, else a
// checkout's (its build/current_helper.txt).
func helperFor(pkg *Set, installed bool) string {
	if h := os.Getenv("STUNTCAM_HELPER"); h != "" {
		return h
	}

	if pkg != nil {
		return pkg.Helper
	}

	if !installed {
		for _, checkout := range checkouts() {
			if checkout.Helper != "" {
				return checkout.Helper
			}
		}
	}

	return ""
}

// rulesFor finds a build dir's restore-rules dir: $STUNTCAM_RULES, else the one two levels up, else the package's, else
// the first checkout's. A staged build's ($SIMCAM_HOME/builds/<h>) two-up $SIMCAM_HOME/restore-rules is the staging
// cache (only <hash>/ dirs), not a rules dir.
func rulesFor(dir string, pkg *Set, installed bool) string {
	if r := os.Getenv("STUNTCAM_RULES"); r != "" {
		return r
	}

	if r := filepath.Join(filepath.Dir(filepath.Dir(dir)), "restore-rules"); !inHome(dir) && exists(r) {
		return r
	}

	if pkg != nil {
		return pkg.Rules
	}

	if !installed {
		if found := checkouts(); len(found) > 0 {
			return found[0].Rules
		}
	}

	return ""
}

func fromCheckout() (Set, error) {
	wd, err := os.Getwd()
	if err != nil {
		return Set{}, err
	}
	return checkoutFrom(wd)
}

var errNoCheckout = errors.New("not in a stuntcam checkout")

// executable is where this binary is. A var for tests (a test binary is built outside any checkout).
var executable = os.Executable

func exeCheckout() (Set, error) {
	exe, err := executable()
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
	for dir := start; ; dir = filepath.Dir(dir) {
		if loaderPointer, err := os.ReadFile(filepath.Join(dir, "build", "current_loader.txt")); err == nil {
			// optional pointers: a missing one leaves its path "" (check requires the stub)
			stub, _ := os.ReadFile(filepath.Join(dir, "build", "current_stub.txt"))
			hook, _ := os.ReadFile(filepath.Join(dir, "build", "current_hook.txt"))
			helper, _ := os.ReadFile(filepath.Join(dir, "build", "current_helper.txt"))
			loader := strings.TrimSpace(string(loaderPointer))
			set := Set{
				Stub:    strings.TrimSpace(string(stub)),
				Hook:    strings.TrimSpace(string(hook)),
				Loader:  loader,
				Engine:  filepath.Join(filepath.Dir(loader), EngineName),
				Profile: filepath.Join(dir, "engine", "profiles", "cfg-m10.plist"),
				Image:   filepath.Join(dir, "fixtures", "media", "qr.png"),
				Canary:  filepath.Join(dir, "canary", "build", "m10canary.app"),
				Rules:   filepath.Join(dir, "restore-rules"),
				From:    dir,
				Helper:  strings.TrimSpace(string(helper)),
			}

			return set, set.check()
		}

		if filepath.Dir(dir) == dir {
			return Set{}, errNoCheckout
		}
	}
}

// dataFiles looks for profiles/cfg-m10.plist and media/qr.png under root, or the checkout layout (engine/profiles,
// fixtures/media).
func dataFiles(root string) (profile, image string) {
	for _, candidate := range []string{
		filepath.Join(root, "profiles", "cfg-m10.plist"),
		filepath.Join(root, "engine", "profiles", "cfg-m10.plist"),
	} {
		if exists(candidate) {
			profile = candidate
			break
		}
	}

	for _, p := range []string{filepath.Join(root, "media", "qr.png"),
		filepath.Join(root, "fixtures", "media", "qr.png")} {
		if exists(p) {
			image = p
			break
		}
	}

	return
}

func newest(glob string) string {
	matches, _ := filepath.Glob(glob)
	var best string
	var bestTime int64
	for _, path := range matches {
		if fi, err := os.Stat(path); err == nil && fi.ModTime().UnixNano() > bestTime {
			best, bestTime = path, fi.ModTime().UnixNano()
		}
	}
	return best
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func (s Set) check() error {
	for _, p := range []string{s.Stub, s.Loader, s.Engine} {
		if p == "" || !exists(p) {
			return fmt.Errorf("missing build product %q (found under %s)", p, s.From)
		}
	}
	return nil
}

var hexDir = regexp.MustCompile(`^[0-9a-f]{12}$`)

// hashLen is the length of a content hash: the first 12 hex digits of a digest, as ./build.sh names its build dirs.
const hashLen = 12

// Stage copies the set under home.Builds()/<hash>/, unless it is already there, and returns the staged set.
// The profile and image go under home.Data()/<hash>/. An armed sim names them by path, so staging another profile
// never changes what it reads.
// The pair dir keeps its name when that is already a 12-hex content hash. Otherwise the name is the sha1 of the engine
// and loader bytes, as ./build.sh computes it: `cat libsimcam.dylib libsimcamloader.dylib | shasum`.
func Stage(set Set) (Set, error) {
	out := set
	pairHash, err := contentName(set.Loader, set.Engine, set.Loader)
	if err != nil {
		return set, err
	}

	stubHash, err := contentName(set.Stub, set.Stub)
	if err != nil {
		return set, err
	}

	if out.Loader, err = copyInto(set.Loader, filepath.Join(home.Builds(), pairHash)); err != nil {
		return set, err
	}

	if out.Engine, err = copyInto(set.Engine, filepath.Join(home.Builds(), pairHash)); err != nil {
		return set, err
	}

	if out.Stub, err = copyInto(set.Stub, filepath.Join(home.Builds(), stubHash)); err != nil {
		return set, err
	}

	if set.Hook != "" && exists(set.Hook) { // its own content-hash dir, like the stub
		hookHash, err := contentName(set.Hook, set.Hook)
		if err != nil {
			return set, err
		}

		if out.Hook, err = copyInto(set.Hook, filepath.Join(home.Builds(), hookHash)); err != nil {
			return set, err
		}
	} else {
		out.Hook = ""
	}

	if out.Profile, err = StageData(set.Profile); err != nil {
		return set, err
	}

	if out.Image, err = StageData(set.Image); err != nil {
		return set, err
	}

	if out.Rules, err = StageRules(set.Rules); err != nil {
		return set, err
	}

	return out, nil
}

// contentName is the content-hash dir name that a staged file goes into. When the file's own dir already has such a
// name (12 hex), that name is kept. Otherwise it is hashFiles of files, which for the pair is the engine, then the
// loader.
func contentName(file string, files ...string) (string, error) {
	if dir := filepath.Base(filepath.Dir(file)); hexDir.MatchString(dir) {
		return dir, nil
	}
	return hashFiles(files...)
}

// RulesHash is the content hash of a restore-rules dir: sha1 over one "<file name> <sha1 hex of its bytes>\n" line per
// [0-9]*.json file in byte order, first 12 hex digits; and those files. scripts/env.sh computes the same hash for the
// same files (`print -r -- "${f:t} $(shasum < $f | cut -c1-40)" | shasum | cut -c1-12`, LC_ALL=C glob order).
func RulesHash(dir string) (string, []string, error) {
	matches, err := filepath.Glob(filepath.Join(dir, "[0-9]*.json")) // sorted, bytewise
	if err != nil {
		return "", nil, err
	}

	var files []string
	hasher := sha1.New()
	for _, file := range matches {
		if info, err := os.Stat(file); err != nil || !info.Mode().IsRegular() {
			continue
		}

		data, err := os.ReadFile(file)
		if err != nil {
			return "", nil, err
		}

		fmt.Fprintf(hasher, "%s %x\n", filepath.Base(file), sha1.Sum(data))
		files = append(files, file)
	}

	return hex.EncodeToString(hasher.Sum(nil))[:hashLen], files, nil
}

// StageRules copies dir's <build>.json files to home.Rules()/<content hash>/ (RulesHash) and returns that dir. The
// sim's config.json names it as rules_dir.
// So a sim armed with one set of rules never sees another clone's files or a later edit, and no stale file feeds the
// loader's "newest" best guess. scripts/env.sh stages dev/<hash>/ the same way.
// The copy is made in a temp dir, which gets a .complete marker and is renamed once complete, so a launch never reads
// half a copy.
// It returns "" when there is no dir or no rules in it.
func StageRules(dir string) (string, error) {
	if dir == "" {
		return "", nil
	}

	rulesHash, files, err := RulesHash(dir)
	if err != nil || len(files) == 0 {
		return "", err
	}

	dst := filepath.Join(home.Rules(), rulesHash)
	if _, err := os.Stat(filepath.Join(dst, ".complete")); err == nil {
		return dst, nil
	}

	if err := os.MkdirAll(home.Rules(), 0o755); err != nil {
		return "", err
	}

	tmp, err := os.MkdirTemp(home.Rules(), ".stage-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(tmp) // gone after a successful rename; a failed stage leaves nothing

	if err := os.Chmod(tmp, 0o755); err != nil {
		return "", err
	}

	for _, file := range files {
		if _, err := copyInto(file, tmp); err != nil {
			return "", err
		}
	}

	if err := os.WriteFile(filepath.Join(tmp, ".complete"), nil, 0o644); err != nil {
		return "", err
	}

	if err := os.Rename(tmp, dst); err != nil {
		if _, e := os.Stat(filepath.Join(dst, ".complete")); e == nil {
			return dst, nil // staged by another stuntcam or script meanwhile: the same content
		}

		return "", err
	}

	return dst, nil
}

// StageData copies a profile, image or video to home.Data()/<sha1-12 of its bytes>/<name> ("" stays ""), where
// simulator processes can read it and a later file with the same name never replaces what an armed sim reads.
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
	hasher := sha1.New()
	for _, p := range paths {
		f, err := os.Open(p)
		if err != nil {
			return "", err
		}

		_, err = io.Copy(hasher, f)
		f.Close() // read only: closing loses nothing
		if err != nil {
			return "", err
		}
	}

	return hex.EncodeToString(hasher.Sum(nil))[:hashLen], nil
}

// copyInto copies src into dir (same base name) unless an identical file is already there, and verifies the copy.
func copyInto(src, dir string) (string, error) {
	dst := filepath.Join(dir, filepath.Base(src))
	// fails only if the working directory is gone
	if a, _ := filepath.Abs(src); a == dst {
		return dst, nil
	}

	want, err := os.ReadFile(src)
	if err != nil {
		return "", err
	}

	if err := putIdentical(dst, want); err != nil {
		if errors.Is(err, errNotIdentical) {
			return "", fmt.Errorf("copy of %s is not identical", src)
		}

		return "", err
	}

	return dst, nil
}

// errNotIdentical means a file written by putIdentical reads back different.
var errNotIdentical = errors.New("not identical")

// putIdentical makes dst hold data (0644, atomically, its dir created), unless it already does, and reads it back.
func putIdentical(dst string, data []byte) error {
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
		return errNotIdentical
	}

	return nil
}

// checkouts returns the checkout around the working directory and the one the executable was built in, those that
// exist and are built. Only a binary that isn't installed asks: fromDir never does for an installed one.
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

// inHome reports whether dir is a build staged under the stuntcam home ($SIMCAM_HOME/builds/<h>), whose neighbours are
// caches rather than a checkout's canary/ and restore-rules/.
func inHome(dir string) bool {
	return filepath.Clean(filepath.Dir(filepath.Dir(dir))) == filepath.Clean(home.Root())
}
