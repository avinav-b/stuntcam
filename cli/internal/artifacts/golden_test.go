// SPDX-License-Identifier: Apache-2.0
package artifacts

import (
	"crypto/sha1"
	"encoding/hex"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// Characterisation tests: the errors Installed returns for each kind of bad payload, and what fromDir resolves for each
// layout, compared with testdata/*.golden. `go test ./internal/artifacts -run Golden -update` rewrites them.

var update = flag.Bool("update", false, "rewrite testdata/*.golden")

func checkGolden(t *testing.T, name, got string) {
	t.Helper()
	path := filepath.Join("testdata", name)
	if *update {
		if err := os.WriteFile(path, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if got != string(want) {
		t.Fatalf("%s differs:\n--- got\n%s\n--- want\n%s", path, got, want)
	}
}

func sha1Hex12(s ...string) string {
	sum := sha1.Sum([]byte(strings.Join(s, "")))
	return hex.EncodeToString(sum[:])[:12]
}

func TestUnpackErrorsGolden(t *testing.T) {
	pair, stub, hook := sha1Hex12("engine", "loader"), sha1Hex12("stub"), sha1Hex12("hook")
	pair2 := sha1Hex12("engine2", "loader2")
	base := [][2]string{
		{"builds/" + pair + "/" + EngineName, "engine"}, {"builds/" + pair + "/" + LoaderName, "loader"},
		{"stub/" + stub + "/" + StubName, "stub"}, {"hook/" + hook + "/" + HookName, "hook"},
		{"canary/m10canary.app/m10canary", "c"},
	}
	with := func(extra ...[2]string) [][2]string { return append(append([][2]string{}, base...), extra...) }
	hm, sha := helperMembers(map[string]string{HelperExe: "x"})
	hm2, _ := helperMembers(map[string]string{HelperExe: "y"})
	noExe, _ := helperMembers(map[string]string{"Contents/Info.plist": "p"})
	var out strings.Builder
	for _, c := range []struct {
		name    string
		members [][2]string
	}{
		{"ok", with()},
		{"ok with a helper", with(hm...)},
		{"no canary", base[:4]},
		{"unexpected member", with([2]string{"bin/other", "x"})},
		{"bad dylib dir name", with([2]string{"builds/zz/" + EngineName, "x"})},
		{"engine without its loader", with([2]string{"builds/" + pair2 + "/" + EngineName, "engine2"})},
		{"two engines", with([2]string{"builds/" + pair2 + "/" + EngineName, "engine2"},
			[2]string{"builds/" + pair2 + "/" + LoaderName, "loader2"})},
		{"two stubs", with([2]string{"stub/" + sha1Hex12("stub2") + "/" + StubName, "stub2"})},
		{"two hooks", with([2]string{"hook/" + sha1Hex12("hook2") + "/" + HookName, "hook2"})},
		{"dylib dir hash", with([2]string{"stub/0123456789ab/" + StubName, "stub3"})},
		{"helper hash", with([2]string{"helper/0123456789ab/" + HelperApp + "/" + HelperExe, "x"})},
		{"helper without its executable", with(noExe...)},
		{"two helpers", with(append(append([][2]string{}, hm...), hm2...)...)},
		{"helper of another name", with([2]string{"helper/" + sha + "/Other.app/" + HelperExe, "x"})},
	} {
		prefix := t.TempDir()
		installedTree(t, prefix, false)
		os.RemoveAll(filepath.Join(prefix, "share"))
		writeInstalled(t, prefix, c.members, map[string]string{"profiles/cfg-m10.plist": "p", "media/qr.png": "i"})
		s, err := Installed(prefix)
		rel := func(p string) string {
			p = strings.ReplaceAll(p, home.Root(), "HOME")
			return strings.ReplaceAll(p, prefix, "PREFIX")
		}
		fmt.Fprintf(&out, "== %s\nerr: %v\n", c.name, err)
		if err == nil {
			fmt.Fprintf(&out, "engine %s\nloader %s\nstub %s\nhook %s\ncanary %s\nhelper %s\n",
				rel(s.Engine), rel(s.Loader), rel(s.Stub), rel(s.Hook), rel(s.Canary), rel(s.Helper))
		}
	}
	checkGolden(t, "unpack.golden", out.String())
}

// fromDir with the build in a release layout, a staged home dir and a bare dir, with and without the package and the
// overriding variables (installed, so no checkout around the test is consulted).
func TestFromDirGolden(t *testing.T) {
	root := t.TempDir()
	t.Setenv("SIMCAM_HOME", filepath.Join(root, "home"))
	for _, v := range []string{"STUNTCAM_STUB", "STUNTCAM_HOOK", "STUNTCAM_PROFILE", "STUNTCAM_IMAGE", "STUNTCAM_CANARY",
		"STUNTCAM_HELPER", "STUNTCAM_RULES"} {
		t.Setenv(v, "")
	}
	// a release layout: the stub and the hook in dirs next to the pair's, the data two levels up
	rel := filepath.Join(root, "release")
	for _, f := range []string{"builds/aaaaaaaaaaaa/" + LoaderName, "builds/aaaaaaaaaaaa/" + EngineName, "builds/s1/" + StubName,
		"builds/h1/" + HookName, "profiles/cfg-m10.plist", "media/qr.png", "canary/m10canary.app/m10canary",
		"restore-rules/27A266a.json"} {
		write(t, filepath.Join(rel, f), "x")
	}
	// a staged build under the home, with no neighbours of its own but home's dev data
	staged := filepath.Join(home.Builds(), "bbbbbbbbbbbb")
	for _, f := range []string{LoaderName, EngineName} {
		write(t, filepath.Join(staged, f), "y")
	}
	write(t, filepath.Join(home.Builds(), "cccccccccccc", StubName), "s")
	write(t, filepath.Join(home.Dev(), "profiles", "cfg-m10.plist"), "p")
	write(t, filepath.Join(home.Dev(), "media", "qr.png"), "i")
	write(t, filepath.Join(home.Root(), "canary", "m10canary.app", "m10canary"), "not the release's")
	write(t, filepath.Join(home.Root(), "restore-rules", "0123456789ab", "27A266a.json"), "{}")
	// a bare dir: no stub anywhere
	bare := filepath.Join(root, "bare", "x", "b")
	for _, f := range []string{LoaderName, EngineName} {
		write(t, filepath.Join(bare, f), "z")
	}
	pkg := &Set{Profile: "/pkg/profile", Image: "/pkg/image", Canary: "/pkg/canary", Helper: "/pkg/helper", Rules: "/pkg/rules"}
	var out strings.Builder
	for _, c := range []struct {
		name      string
		dir       string
		pkg       *Set
		installed bool
		env       map[string]string
	}{
		{name: "release", dir: filepath.Join(rel, "builds", "aaaaaaaaaaaa"), installed: true},
		{name: "release, installed with package", dir: filepath.Join(rel, "builds", "aaaaaaaaaaaa"), pkg: pkg, installed: true},
		{name: "staged, installed with package", dir: staged, pkg: pkg, installed: true},
		{name: "staged, installed without package", dir: staged, installed: true},
		{name: "staged, every variable", dir: staged, installed: true, env: map[string]string{
			"STUNTCAM_STUB": "/e/stub", "STUNTCAM_HOOK": "/e/hook", "STUNTCAM_PROFILE": filepath.Join(home.Dev(), "profiles",
				"cfg-m10.plist"), "STUNTCAM_IMAGE": "/e/image", "STUNTCAM_CANARY": "/e/canary", "STUNTCAM_HELPER": "/e/helper",
			"STUNTCAM_RULES": "/e/rules"}},
		{name: "staged, profile variable names a missing file", dir: staged, installed: true,
			env: map[string]string{"STUNTCAM_PROFILE": "/e/none"}},
		{name: "bare", dir: bare, installed: true},
	} {
		for k, v := range c.env {
			t.Setenv(k, v)
		}
		s, err := fromDir(c.dir, c.pkg, c.installed)
		for k := range c.env {
			t.Setenv(k, "")
		}
		r := func(p string) string { return strings.ReplaceAll(p, root, "ROOT") }
		fmt.Fprintf(&out, "== %s\nerr: %v\n", c.name, r(fmt.Sprint(err)))
		for _, f := range [][2]string{{"from", s.From}, {"loader", s.Loader}, {"engine", s.Engine}, {"stub", s.Stub},
			{"hook", s.Hook}, {"profile", s.Profile}, {"image", s.Image}, {"canary", s.Canary}, {"helper", s.Helper},
			{"rules", s.Rules}} {
			fmt.Fprintf(&out, "%s %s\n", f[0], r(f[1]))
		}
	}
	checkGolden(t, "fromdir.golden", out.String())
}

// TestFromDirCheckoutFallbacks: for a binary that isn't installed and a --build dir with no package and no neighbours
// of its own (a staged build), the canary, the helper and the rules come from a checkout: the one around the working
// directory first, then the one the executable was built in. The canary is the checkout's whose current build is that
// dir's; the helper is the first checkout's that has one; the rules are the first checkout's.
func TestFromDirCheckoutFallbacks(t *testing.T) {
	root := t.TempDir()
	if r, err := filepath.EvalSymlinks(root); err == nil {
		root = r
	}
	t.Setenv("SIMCAM_HOME", filepath.Join(root, "home"))
	for _, v := range []string{"STUNTCAM_STUB", "STUNTCAM_HOOK", "STUNTCAM_PROFILE", "STUNTCAM_IMAGE", "STUNTCAM_CANARY",
		"STUNTCAM_HELPER", "STUNTCAM_RULES"} {
		t.Setenv(v, "")
	}
	// three staged builds under the home: each checkout's current one, and one that is nobody's
	wdBuild := filepath.Join(home.Builds(), "aaaaaaaaaaaa")
	exeBuild := filepath.Join(home.Builds(), "bbbbbbbbbbbb")
	other := filepath.Join(home.Builds(), "dddddddddddd")
	for _, d := range []string{wdBuild, exeBuild, other} {
		write(t, filepath.Join(d, LoaderName), "l")
		write(t, filepath.Join(d, EngineName), "e")
	}
	stub := filepath.Join(home.Builds(), "cccccccccccc", StubName)
	write(t, stub, "s")
	write(t, filepath.Join(home.Dev(), "profiles", "cfg-m10.plist"), "p")
	// the checkouts: their pointer files name the staged copies, as ./build.sh writes them. Only the executable's has
	// built a helper.
	wdCheckout := filepath.Join(root, "wd-checkout")
	write(t, filepath.Join(wdCheckout, "build", "current_loader.txt"), filepath.Join(wdBuild, LoaderName)+"\n")
	write(t, filepath.Join(wdCheckout, "build", "current_stub.txt"), stub+"\n")
	exeCheckoutDir := filepath.Join(root, "exe-checkout")
	write(t, filepath.Join(exeCheckoutDir, "build", "current_loader.txt"), filepath.Join(exeBuild, LoaderName)+"\n")
	write(t, filepath.Join(exeCheckoutDir, "build", "current_stub.txt"), stub+"\n")
	write(t, filepath.Join(exeCheckoutDir, "build", "current_helper.txt"), "/h/Stuntcam Camera.app\n")
	write(t, filepath.Join(exeCheckoutDir, "cli", "bin", "stuntcam"), "x")
	oldExecutable := executable
	defer func() { executable = oldExecutable }()
	executable = func() (string, error) { return filepath.Join(exeCheckoutDir, "cli", "bin", "stuntcam"), nil }
	wd, _ := os.Getwd()
	defer os.Chdir(wd)
	if err := os.Chdir(filepath.Join(wdCheckout, "build")); err != nil {
		t.Fatal(err)
	}
	for _, c := range []struct {
		name, dir, canary string
	}{
		{"the working directory's checkout's build", wdBuild, filepath.Join(wdCheckout, "canary", "build", "m10canary.app")},
		{"the executable's checkout's build", exeBuild, filepath.Join(exeCheckoutDir, "canary", "build", "m10canary.app")},
		{"another build", other, ""},
	} {
		s, err := fromDir(c.dir, nil, false)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if s.Canary != c.canary || s.Helper != "/h/Stuntcam Camera.app" || s.Rules != filepath.Join(wdCheckout, "restore-rules") {
			t.Errorf("%s: canary %q helper %q rules %q", c.name, s.Canary, s.Helper, s.Rules)
		}
	}
	// with a helper in both checkouts, the working directory's (the first checkout's) wins
	write(t, filepath.Join(wdCheckout, "build", "current_helper.txt"), "/wd/Stuntcam Camera.app\n")
	if s, err := fromDir(other, nil, false); err != nil || s.Helper != "/wd/Stuntcam Camera.app" {
		t.Errorf("two helpers: helper %q err %v, want the working directory's checkout's", s.Helper, err)
	}
	// an installed binary never looks at a checkout
	if s, err := fromDir(wdBuild, nil, true); err != nil || s.Canary != "" || s.Helper != "" || s.Rules != "" {
		t.Errorf("installed: canary %q helper %q rules %q err %v", s.Canary, s.Helper, s.Rules, err)
	}
}
