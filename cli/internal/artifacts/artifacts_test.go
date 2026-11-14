// SPDX-License-Identifier: Apache-2.0
package artifacts

import (
	"archive/tar"
	"bytes"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

func write(t *testing.T, p, s string) {
	t.Helper()
	os.MkdirAll(filepath.Dir(p), 0o755)
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestStageKeepsHashDirsAndIsIdempotent(t *testing.T) {
	src := t.TempDir()
	t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
	s := Set{Stub: filepath.Join(src, "485dc3d39b99", StubName), Loader: filepath.Join(src, "06b3c790be86", LoaderName),
		Engine: filepath.Join(src, "06b3c790be86", EngineName), Profile: filepath.Join(src, "p", "cfg-m10.plist"), Image: filepath.Join(src, "m", "qr.png")}
	write(t, s.Stub, "stub")
	write(t, s.Loader, "loader")
	write(t, s.Engine, "engine")
	write(t, s.Profile, "plist")
	write(t, s.Image, "png")
	out, err := Stage(s)
	if err != nil {
		t.Fatal(err)
	}
	if out.Loader != filepath.Join(home.Builds(), "06b3c790be86", LoaderName) || out.Stub != filepath.Join(home.Builds(), "485dc3d39b99", StubName) {
		t.Fatalf("%+v", out)
	}
	if out.BuildID() != "06b3c790be86" {
		t.Fatal(out.BuildID())
	}
	if fi, _ := os.Stat(out.Engine); fi.Mode().Perm() != 0o644 {
		t.Fatalf("mode %v", fi.Mode())
	}
	again, err := Stage(out) // staging the staged set is a no-op
	if err != nil || again != out {
		t.Fatalf("%+v %v", again, err)
	}
}

func TestStageHashesNonHashDirs(t *testing.T) {
	src := t.TempDir()
	t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
	s := Set{Stub: filepath.Join(src, "stub", StubName), Loader: filepath.Join(src, "pair", LoaderName), Engine: filepath.Join(src, "pair", EngineName)}
	write(t, s.Stub, "stub")
	write(t, s.Loader, "loader")
	write(t, s.Engine, "engine")
	out, err := Stage(s)
	if err != nil {
		t.Fatal(err)
	}
	sum := sha1.Sum([]byte("engineloader")) // ./build.sh: cat libsimcam.dylib libsimcamloader.dylib | shasum
	want := hex.EncodeToString(sum[:])[:12]
	if out.BuildID() != want {
		t.Fatalf("%s vs %s", out.BuildID(), want)
	}
}

func TestFindFromDirNeedsAllThree(t *testing.T) {
	d := t.TempDir()
	// isolated from the host: fromDir falls back to $SIMCAM_HOME/dev's profile, which any Mac that ran ./build.sh has (w3 e18)
	t.Setenv("SIMCAM_HOME", filepath.Join(d, "home"))
	t.Setenv("STUNTCAM_PROFILE", "")
	t.Setenv("STUNTCAM_IMAGE", "")
	write(t, filepath.Join(d, "b1", LoaderName), "l")
	if _, err := fromDir(filepath.Join(d, "b1"), nil, false); err == nil {
		t.Fatal("accepted a dir without engine or stub")
	}
	write(t, filepath.Join(d, "b1", EngineName), "e")
	write(t, filepath.Join(d, "s1", StubName), "s")
	if _, err := fromDir(filepath.Join(d, "b1"), nil, false); err == nil {
		t.Fatal("accepted a build without a profile (apps crash without M2_PLIST)")
	}
	write(t, filepath.Join(d, "p", "cfg-m10.plist"), "p")
	t.Setenv("STUNTCAM_PROFILE", filepath.Join(d, "p", "cfg-m10.plist"))
	s, err := fromDir(filepath.Join(d, "b1"), nil, false)
	if err != nil || s.Stub != filepath.Join(d, "s1", StubName) || s.Profile != filepath.Join(d, "p", "cfg-m10.plist") {
		t.Fatalf("%+v %v", s, err)
	}
}

func TestStageDataIsContentAddressed(t *testing.T) {
	// w3 e13: a refused `up` with another profile overwrote dev/profiles/cfg-m10.plist, which an armed sim's M2_PLIST named
	src := t.TempDir()
	t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
	write(t, filepath.Join(src, "a", "cfg-m10.plist"), "good")
	write(t, filepath.Join(src, "b", "cfg-m10.plist"), "broken")
	a, err := StageData(filepath.Join(src, "a", "cfg-m10.plist"))
	if err != nil {
		t.Fatal(err)
	}
	b, err := StageData(filepath.Join(src, "b", "cfg-m10.plist"))
	if err != nil {
		t.Fatal(err)
	}
	if a == b || filepath.Base(a) != "cfg-m10.plist" || filepath.Dir(filepath.Dir(a)) != home.Data() {
		t.Fatalf("%s %s", a, b)
	}
	if got, _ := os.ReadFile(a); string(got) != "good" {
		t.Fatalf("first profile changed: %q", got)
	}
	if again, _ := StageData(a); again != a {
		t.Fatalf("restaging moved it: %s", again)
	}
}

func TestBrokenCheckoutIsAnError(t *testing.T) {
	d := t.TempDir()
	os.MkdirAll(filepath.Join(d, "build"), 0o755)
	os.WriteFile(filepath.Join(d, "build", "current_loader.txt"), []byte("/gone/libsimcamloader.dylib\n"), 0o644)
	wd, _ := os.Getwd()
	defer os.Chdir(wd)
	os.Chdir(d)
	t.Setenv("STUNTCAM_BUILD", "")
	if _, err := Find(""); err == nil || errors.Is(err, errNoCheckout) {
		t.Fatalf("a checkout with a missing build must be reported, not skipped: %v", err)
	}
}

func TestStageRulesPerContent(t *testing.T) {
	// w4 e30 (M2): rules were staged into one shared restore-rules/ and overwritten by the next clone or edit, under armed sims
	src := t.TempDir()
	t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
	a, b := filepath.Join(src, "a"), filepath.Join(src, "b")
	write(t, filepath.Join(a, "27A266a.json"), `{"rules": [1]}`)
	write(t, filepath.Join(a, "README.md"), "not a rules file")
	write(t, filepath.Join(b, "27A266a.json"), `{"rules": [2]}`)
	write(t, filepath.Join(b, "27B1.json"), `{"rules": [3]}`)
	da, err := StageRules(a)
	if err != nil {
		t.Fatal(err)
	}
	db, err := StageRules(b)
	if err != nil {
		t.Fatal(err)
	}
	if da == db || filepath.Dir(da) != home.Rules() || len(filepath.Base(da)) != 12 {
		t.Fatalf("%s %s", da, db)
	}
	if got, _ := os.ReadFile(filepath.Join(da, "27A266a.json")); string(got) != `{"rules": [1]}` {
		t.Fatalf("first clone's rules changed: %q", got)
	}
	if _, err := os.Stat(filepath.Join(da, "27B1.json")); err == nil {
		t.Fatal("another clone's newer rules file leaked into this dir (it would feed the newest best guess)")
	}
	if _, err := os.Stat(filepath.Join(da, "README.md")); err == nil {
		t.Fatal("README staged")
	}
	if _, err := os.Stat(filepath.Join(da, ".complete")); err != nil {
		t.Fatal("no .complete marker")
	}
	if again, err := StageRules(a); err != nil || again != da {
		t.Fatalf("restaging: %s %v", again, err)
	}
	if left, _ := filepath.Glob(filepath.Join(home.Rules(), ".stage-*")); len(left) != 0 {
		t.Fatalf("temp dirs left: %v", left)
	}
	// an edit gives a new dir; the old one stays as it was for the sims that name it
	write(t, filepath.Join(a, "27A266a.json"), `{"rules": [1, "fixed"]}`)
	if d2, _ := StageRules(a); d2 == da {
		t.Fatal("edited rules staged under the old hash")
	}
	if got, _ := os.ReadFile(filepath.Join(da, "27A266a.json")); string(got) != `{"rules": [1]}` {
		t.Fatalf("edit reached the armed dir: %q", got)
	}
	if d, err := StageRules(t.TempDir()); d != "" || err != nil {
		t.Fatalf("empty dir: %q %v", d, err)
	}
	if d, err := StageRules(""); d != "" || err != nil {
		t.Fatalf("no dir: %q %v", d, err)
	}
}

// scripts/env.sh stages the same files under the same hash (its pipeline, verbatim)
func TestRulesHashMatchesEnvSh(t *testing.T) {
	if _, err := exec.LookPath("zsh"); err != nil {
		t.Skip("no zsh")
	}
	if _, err := exec.LookPath("shasum"); err != nil {
		t.Skip("no shasum")
	}
	d := t.TempDir()
	write(t, filepath.Join(d, "27A266a.json"), `{"rules": []}`)
	write(t, filepath.Join(d, "26E5.json"), "{\"rules\": [\"x\"]}\n")
	write(t, filepath.Join(d, "README.md"), "x")
	want, _, err := RulesHash(d)
	if err != nil {
		t.Fatal(err)
	}
	out, err := exec.Command("zsh", "-fc", `local f LC_ALL=C; for f in $1/[0-9]*.json(.N); do print -r -- "${f:t} $(shasum < $f | cut -c1-40)"; done | shasum | cut -c1-12`, "zsh", d).Output()
	if err != nil {
		t.Fatal(err)
	}
	if got := strings.TrimSpace(string(out)); got != want {
		t.Fatalf("env.sh hash %s, CLI hash %s", got, want)
	}
}

// the runner hook (w4 e42) is staged into its own content-hash dir like the stub, and is optional: a build without one stages
// with Hook "" (exec then warns instead of hooking)
func TestStageHook(t *testing.T) {
	src := t.TempDir()
	t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
	s := Set{Stub: filepath.Join(src, "485dc3d39b99", StubName), Loader: filepath.Join(src, "06b3c790be86", LoaderName),
		Engine: filepath.Join(src, "06b3c790be86", EngineName), Hook: filepath.Join(src, "hookdir", HookName)}
	write(t, s.Stub, "stub")
	write(t, s.Loader, "loader")
	write(t, s.Engine, "engine")
	write(t, s.Hook, "hook")
	out, err := Stage(s)
	if err != nil {
		t.Fatal(err)
	}
	want, _ := hashFiles(s.Hook)
	if out.Hook != filepath.Join(home.Builds(), want, HookName) {
		t.Fatalf("%q", out.Hook)
	}
	s.Hook = filepath.Join(src, "gone", HookName)
	if out, err = Stage(s); err != nil || out.Hook != "" {
		t.Fatalf("%q %v", out.Hook, err)
	}
}

func TestCheckoutFindsHook(t *testing.T) {
	d := t.TempDir()
	for _, f := range []string{"b/libsimcamloader.dylib", "b/libsimcam.dylib", "s/libsimcamstub.dylib", "k/libsimcamrunnerhook.dylib"} {
		write(t, filepath.Join(d, f), "x")
	}
	write(t, filepath.Join(d, "build", "current_loader.txt"), filepath.Join(d, "b", LoaderName)+"\n")
	write(t, filepath.Join(d, "build", "current_stub.txt"), filepath.Join(d, "s", StubName)+"\n")
	s, err := checkoutFrom(d)
	if err != nil || s.Hook != "" {
		t.Fatalf("no current_hook.txt: %q %v", s.Hook, err)
	}
	write(t, filepath.Join(d, "build", "current_hook.txt"), filepath.Join(d, "k", HookName)+"\n")
	if s, err = checkoutFrom(d); err != nil || s.Hook != filepath.Join(d, "k", HookName) {
		t.Fatalf("%q %v", s.Hook, err)
	}
}

// installedTree writes an installed layout (scripts/package.sh) under prefix: the payload tar with correctly named hash
// dirs, the loose data files and a manifest of all of them. It sets SIMCAM_HOME to a fresh dir (where Installed unpacks).
func installedTree(t *testing.T, prefix string, withVerdicts bool) (pair, stub, hook string) {
	t.Helper()
	t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
	h := func(b ...string) string {
		s := sha1.Sum([]byte(strings.Join(b, "")))
		return hex.EncodeToString(s[:])[:12]
	}
	pair, stub, hook = h("engine", "loader"), h("stub"), h("hook")
	members := [][2]string{
		{"builds/" + pair + "/" + EngineName, "engine"}, {"builds/" + pair + "/" + LoaderName, "loader"},
		{"stub/" + stub + "/" + StubName, "stub"}, {"hook/" + hook + "/" + HookName, "hook"},
		{"canary/m10canary.app/m10canary", "c"}, {"canary/m10canary.app/Info.plist", "plist"},
	}
	loose := map[string]string{"profiles/cfg-m10.plist": "p", "media/qr.png": "i", "restore-rules/27A266a.json": "{}"}
	if withVerdicts {
		loose["verdicts/k.json"] = "{}"
	}
	writeInstalled(t, prefix, members, loose)
	return
}

// writeInstalled writes payload.tar from members (name, content), the loose share files and the manifest.
func writeInstalled(t *testing.T, prefix string, members [][2]string, loose map[string]string) {
	t.Helper()
	share := filepath.Join(prefix, "share", "stuntcam")
	os.MkdirAll(share, 0o755)
	var lines []string
	sum := func(b []byte) string { s := sha256.Sum256(b); return hex.EncodeToString(s[:]) }
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	for _, m := range members {
		mode := int64(0o644)
		if strings.HasSuffix(m[0], "/m10canary") {
			mode = 0o755
		}
		tw.WriteHeader(&tar.Header{Name: m[0], Mode: mode, Size: int64(len(m[1])), Typeflag: tar.TypeReg})
		tw.Write([]byte(m[1]))
		lines = append(lines, sum([]byte(m[1]))+"  payload/"+m[0])
	}
	tw.Close()
	write(t, filepath.Join(share, PayloadName), buf.String())
	lines = append(lines, sum(buf.Bytes())+"  share/stuntcam/"+PayloadName)
	for p, c := range loose {
		write(t, filepath.Join(share, p), c)
		lines = append(lines, sum([]byte(c))+"  share/stuntcam/"+p)
	}
	write(t, filepath.Join(prefix, "bin", "stuntcam"), "#!/bin/sh\n")
	lines = append(lines, sum([]byte("#!/bin/sh\n"))+"  bin/stuntcam")
	write(t, filepath.Join(share, ManifestName), "stuntcam 9.9.9 commit abc\nengine+loader x stub y\n"+strings.Join(lines, "\n")+"\n")
}

func TestInstalledLayout(t *testing.T) {
	prefix := t.TempDir()
	pair, stub, hook := installedTree(t, prefix, true)
	s, err := Installed(prefix)
	if err != nil {
		t.Fatal(err)
	}
	share := filepath.Join(prefix, "share", "stuntcam")
	// the Mach-O files are unpacked into the host directory, under their content-hash names
	if !s.Installed || s.From != share || s.BuildID() != pair || s.Version != "9.9.9 (commit abc)" ||
		s.Engine != filepath.Join(home.Builds(), pair, EngineName) || s.Stub != filepath.Join(home.Builds(), stub, StubName) ||
		s.Hook != filepath.Join(home.Builds(), hook, HookName) {
		t.Fatalf("%+v", s)
	}
	if b, _ := os.ReadFile(s.Engine); string(b) != "engine" {
		t.Fatalf("engine %q", b)
	}
	if filepath.Dir(filepath.Dir(s.Canary)) != home.Canary() || filepath.Base(s.Canary) != "m10canary.app" {
		t.Fatalf("canary %s", s.Canary)
	}
	if fi, err := os.Stat(filepath.Join(s.Canary, "m10canary")); err != nil || fi.Mode().Perm() != 0o755 {
		t.Fatalf("canary exec bit: %v %v", fi, err)
	}
	if s.Profile != filepath.Join(share, "profiles", "cfg-m10.plist") || s.Image != filepath.Join(share, "media", "qr.png") ||
		s.Rules != filepath.Join(share, "restore-rules") || s.Verdicts != filepath.Join(share, "verdicts") {
		t.Fatalf("%+v", s)
	}
	// staging is a no-op for the dylibs (already there), so verdict keys and sentinel build ids match the package's
	out, err := Stage(s)
	if err != nil || out.BuildID() != pair || out.Stub != s.Stub || out.Hook != s.Hook || out.Engine != s.Engine {
		t.Fatalf("%+v %v", out, err)
	}
	// a second use re-checks and keeps what is there
	if again, err := Installed(prefix); err != nil || again != s {
		t.Fatalf("%+v %v", again, err)
	}
	// an unpacked copy that changed is put back from the payload
	write(t, s.Loader, "tampered")
	write(t, filepath.Join(s.Canary, "m10canary"), "tampered")
	if again, err := Installed(prefix); err != nil || again != s {
		t.Fatalf("%+v %v", again, err)
	}
	if b, _ := os.ReadFile(s.Loader); string(b) != "loader" {
		t.Fatalf("loader not restored: %q", b)
	}
	if b, _ := os.ReadFile(filepath.Join(s.Canary, "m10canary")); string(b) != "c" {
		t.Fatalf("canary not restored: %q", b)
	}
	// review (4): a canary executable that lost its exec bit (same bytes) is unpacked again
	os.Chmod(filepath.Join(s.Canary, "m10canary"), 0o644)
	if again, err := Installed(prefix); err != nil || again != s {
		t.Fatalf("%+v %v", again, err)
	}
	if fi, err := os.Stat(filepath.Join(s.Canary, "m10canary")); err != nil || fi.Mode().Perm() != 0o755 {
		t.Fatalf("canary exec bit not restored: %v %v", fi, err)
	}
	// and a data file that gained one is too
	os.Chmod(filepath.Join(s.Canary, "Info.plist"), 0o755)
	if again, err := Installed(prefix); err != nil || again != s {
		t.Fatalf("%+v %v", again, err)
	}
	if fi, _ := os.Stat(filepath.Join(s.Canary, "Info.plist")); fi.Mode().Perm() != 0o644 {
		t.Fatalf("Info.plist mode %v", fi.Mode())
	}
	if left, _ := filepath.Glob(filepath.Join(home.Canary(), ".*")); len(left) != 1 || filepath.Base(left[0]) != ".lock" {
		t.Fatalf("temp dirs left: %v", left)
	}
}

func TestInstalledWithoutVerdictsOrHook(t *testing.T) {
	prefix := t.TempDir()
	pair, stub, _ := installedTree(t, prefix, false)
	os.RemoveAll(filepath.Join(prefix, "share"))
	writeInstalled(t, prefix, [][2]string{
		{"builds/" + pair + "/" + EngineName, "engine"}, {"builds/" + pair + "/" + LoaderName, "loader"},
		{"stub/" + stub + "/" + StubName, "stub"}, {"canary/m10canary.app/m10canary", "c"},
	}, map[string]string{"profiles/cfg-m10.plist": "p", "media/qr.png": "i"})
	s, err := Installed(prefix)
	if err != nil || s.Verdicts != "" || s.Hook != "" {
		t.Fatalf("%+v %v", s, err)
	}
	// w4 e51: doctor counted the working directory's *.json (package.json) as shipped verdicts when there were none
	wd, _ := os.Getwd()
	defer os.Chdir(wd)
	d := t.TempDir()
	write(t, filepath.Join(d, "package.json"), "{}")
	os.Chdir(d)
	if n := s.ShippedVerdicts(); len(n) != 0 {
		t.Fatalf("no verdicts dir: %v", n)
	}
	if s, _ = Installed(func() string { p := t.TempDir(); installedTree(t, p, true); return p }()); len(s.ShippedVerdicts()) != 1 {
		t.Fatalf("one shipped verdict: %v", s.ShippedVerdicts())
	}
}

// Every file in the manifest is checked at use (review): restore rules, media, profile, verdicts and the payload,
// not only the dylibs; an added or missing file too.
func TestInstalledRejectsChangedFiles(t *testing.T) {
	for _, c := range []struct {
		name string
		edit func(share string)
		want string
	}{
		{"changed restore-rules file", func(sh string) { write(t, filepath.Join(sh, "restore-rules", "27A266a.json"), `{"rules": ["evil"]}`) }, "restore-rules/27A266a.json has sha256"},
		{"added restore-rules file", func(sh string) { write(t, filepath.Join(sh, "restore-rules", "27Z1.json"), "{}") }, "restore-rules/27Z1.json is not in manifest.txt"},
		{"changed media", func(sh string) { write(t, filepath.Join(sh, "media", "qr.png"), "x") }, "media/qr.png has sha256"},
		{"changed profile", func(sh string) { write(t, filepath.Join(sh, "profiles", "cfg-m10.plist"), "x") }, "profiles/cfg-m10.plist has sha256"},
		{"changed verdict", func(sh string) { write(t, filepath.Join(sh, "verdicts", "k.json"), `{"verdict":"pass"}`) }, "verdicts/k.json has sha256"},
		{"added verdict", func(sh string) { write(t, filepath.Join(sh, "verdicts", "x.json"), "{}") }, "verdicts/x.json is not in manifest.txt"},
		{"missing profile", func(sh string) { os.Remove(filepath.Join(sh, "profiles", "cfg-m10.plist")) }, "profiles/cfg-m10.plist is missing"},
		{"changed payload", func(sh string) {
			b, _ := os.ReadFile(filepath.Join(sh, PayloadName))
			write(t, filepath.Join(sh, PayloadName), strings.Replace(string(b), "engine", "enginX", 1))
		}, "payload.tar has sha256"},
		{"loose dylib next to the payload", func(sh string) { write(t, filepath.Join(sh, "builds", "x", EngineName), "e") }, "builds/x/libsimcam.dylib is not in manifest.txt"},
		{"symlink", func(sh string) { os.Symlink("/etc/hosts", filepath.Join(sh, "media", "link")) }, "media/link is not a regular file"},
	} {
		prefix := t.TempDir()
		installedTree(t, prefix, true)
		c.edit(filepath.Join(prefix, "share", "stuntcam"))
		if _, err := Installed(prefix); err == nil || !strings.Contains(err.Error(), "changed after packaging") || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: %v", c.name, err)
		}
	}
}

// A payload whose member was rewritten (a relinked dylib re-tarred, with the tar's own manifest line updated to match):
// the member's sha256 and its content-hash dir name both catch it.
func TestInstalledRejectsChangedPayloadMember(t *testing.T) {
	prefix := t.TempDir()
	pair, stub, _ := installedTree(t, prefix, false)
	share := filepath.Join(prefix, "share", "stuntcam")
	man, _ := os.ReadFile(filepath.Join(share, ManifestName))
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	for _, m := range [][2]string{{"builds/" + pair + "/" + EngineName, "relinked"}, {"builds/" + pair + "/" + LoaderName, "loader"},
		{"stub/" + stub + "/" + StubName, "stub"}, {"canary/m10canary.app/m10canary", "c"}, {"canary/m10canary.app/Info.plist", "plist"}} {
		tw.WriteHeader(&tar.Header{Name: m[0], Mode: 0o644, Size: int64(len(m[1])), Typeflag: tar.TypeReg})
		tw.Write([]byte(m[1]))
	}
	tw.Close()
	write(t, filepath.Join(share, PayloadName), buf.String())
	sum := sha256.Sum256(buf.Bytes())
	var out []string
	for _, l := range strings.Split(strings.TrimSpace(string(man)), "\n") {
		if strings.HasSuffix(l, "  share/stuntcam/"+PayloadName) {
			l = hex.EncodeToString(sum[:]) + "  share/stuntcam/" + PayloadName
		}
		if !strings.Contains(l, "payload/hook/") {
			out = append(out, l)
		}
	}
	write(t, filepath.Join(share, ManifestName), strings.Join(out, "\n")+"\n")
	_, err := Installed(prefix)
	if err == nil || !strings.Contains(err.Error(), "member builds/"+pair+"/"+EngineName+" doesn't match") {
		t.Fatalf("%v", err)
	}
	if exists(filepath.Join(home.Builds(), pair)) {
		t.Fatal("unpacked a payload that failed its check")
	}
}

func TestInstalledRejectsOddPayloadMembers(t *testing.T) {
	for _, m := range [][2]string{{"../escape.dylib", "x"}, {"builds/zz/" + EngineName, "x"}, {"bin/other", "x"}} {
		prefix := t.TempDir()
		installedTree(t, prefix, false)
		os.RemoveAll(filepath.Join(prefix, "share"))
		pair := sha1.Sum([]byte("engineloader"))
		stub := sha1.Sum([]byte("stub"))
		writeInstalled(t, prefix, [][2]string{
			{"builds/" + hex.EncodeToString(pair[:])[:12] + "/" + EngineName, "engine"}, {"builds/" + hex.EncodeToString(pair[:])[:12] + "/" + LoaderName, "loader"},
			{"stub/" + hex.EncodeToString(stub[:])[:12] + "/" + StubName, "stub"}, {"canary/m10canary.app/m10canary", "c"}, m,
		}, map[string]string{"profiles/cfg-m10.plist": "p", "media/qr.png": "i"})
		if _, err := Installed(prefix); err == nil || !strings.Contains(err.Error(), "changed after packaging") && !strings.Contains(err.Error(), "bad entry") {
			t.Errorf("%s: %v", m[0], err)
		}
	}
}

func TestInstalledMissingIsNotALayout(t *testing.T) {
	if _, err := Installed(t.TempDir()); !errors.Is(err, errNoCheckout) {
		t.Fatalf("no share/stuntcam: %v", err)
	}
	prefix := t.TempDir()
	installedTree(t, prefix, false)
	os.Remove(filepath.Join(prefix, "share", "stuntcam", ManifestName))
	if _, err := Installed(prefix); err == nil || errors.Is(err, errNoCheckout) {
		t.Fatalf("incomplete layout must be an error: %v", err)
	}
}

// The binary runs through a symlink (node_modules/.bin/stuntcam, $(brew --prefix)/bin/stuntcam): Find follows it.
func TestFindFollowsSymlinkedExecutable(t *testing.T) {
	prefix := t.TempDir()
	pair, _, _ := installedTree(t, prefix, false)
	bin := t.TempDir()
	if err := exec.Command("go", "build", "-o", filepath.Join(prefix, "bin", "probe"), "./testdata/probe").Run(); err != nil {
		t.Fatalf("go build: %v", err)
	}
	if err := os.Symlink(filepath.Join(prefix, "bin", "probe"), filepath.Join(bin, "stuntcam")); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(filepath.Join(bin, "stuntcam"))
	cmd.Dir = t.TempDir() // not in a checkout
	o, err := cmd.CombinedOutput()
	if err != nil || strings.TrimSpace(string(o)) != pair+" installed" {
		t.Fatalf("%s %v", o, err)
	}
}

// fakeCheckout writes a checkout at d whose build pointers name dylibs under home.Builds() (as ./build.sh stages them).
// real: with build.sh and engine/src/simcam.m. It returns the pair hash.
func fakeCheckout(t *testing.T, d string, real bool) string {
	t.Helper()
	h := func(b string) string { s := sha1.Sum([]byte(b)); return hex.EncodeToString(s[:])[:12] }
	pair, stub := h("E2L2"), h("S2")
	write(t, filepath.Join(home.Builds(), pair, EngineName), "E2")
	write(t, filepath.Join(home.Builds(), pair, LoaderName), "L2")
	write(t, filepath.Join(home.Builds(), stub, StubName), "S2")
	write(t, filepath.Join(d, "build", "current_loader.txt"), filepath.Join(home.Builds(), pair, LoaderName)+"\n")
	write(t, filepath.Join(d, "build", "current_dylib.txt"), filepath.Join(home.Builds(), pair, EngineName)+"\n")
	write(t, filepath.Join(d, "build", "current_stub.txt"), filepath.Join(home.Builds(), stub, StubName)+"\n")
	if real {
		write(t, filepath.Join(d, "build.sh"), "#!/bin/zsh\n")
		write(t, filepath.Join(d, "engine", "src", "simcam.m"), "//\n")
	}
	return pair
}

// review: an installed stuntcam takes a working-directory checkout only with STUNTCAM_USE_CHECKOUT=1, and then
// only when it is a real stuntcam root whose pointers resolve under home.Builds() to content-hash dirs; anything else is
// ignored (with a one-line note for a stuntcam checkout or an opt-in).
func TestInstalledCheckoutTrust(t *testing.T) {
	var notes []string
	old := Note
	Note = func(m string) { notes = append(notes, m) }
	defer func() { Note = old }()
	noteSeen = map[string]bool{}
	t.Setenv("STUNTCAM_BUILD", "")
	t.Setenv(UseCheckoutEnv, "")

	prefix := t.TempDir()
	installed, _, _ := installedTree(t, prefix, false)
	pkg, err := Installed(prefix)
	if err != nil {
		t.Fatal(err)
	}
	find1 := func(wd string) (Set, []string) {
		t.Helper()
		notes = nil
		noteSeen = map[string]bool{}
		s, err := find("", prefix, wd)
		if err != nil {
			t.Fatalf("%s: %v", wd, err)
		}
		return s, notes
	}
	use := func(wd string) (Set, string) {
		t.Helper()
		s, n := find1(wd)
		if len(n) != 1 {
			t.Fatalf("%s: want one note, got %q", wd, n)
		}
		return s, n[0]
	}

	// nothing in the working directory: the installed build, no note
	if s, n := find1(t.TempDir()); !s.Installed || s.BuildID() != installed || len(n) != 0 {
		t.Fatalf("%+v %q", s, n)
	}

	// without the opt-in: a project that just has build/current_loader.txt is ignored silently, a real checkout with a
	// good build is ignored with a note naming the opt-in
	d := t.TempDir()
	write(t, filepath.Join(d, "build", "current_loader.txt"), "/tmp/evil/libsimcamloader.dylib\n")
	if s, n := find1(filepath.Join(d, "sub")); !s.Installed || len(n) != 0 {
		t.Fatalf("%+v %q", s, n)
	}
	good := t.TempDir()
	goodPair := fakeCheckout(t, good, true)
	if s, n := use(filepath.Join(good, "engine")); !s.Installed || s.BuildID() != installed ||
		!strings.Contains(n, "ignoring the stuntcam checkout in "+good) || !strings.Contains(n, UseCheckoutEnv+"=1") {
		t.Fatalf("%+v %q", s, n)
	}
	t.Setenv(UseCheckoutEnv, "0")
	if s, _ := use(good); !s.Installed {
		t.Fatalf("%s=0 is not an opt-in: %+v", UseCheckoutEnv, s)
	}

	t.Setenv(UseCheckoutEnv, "1")
	// opted in, a project that just has build/current_loader.txt (not a stuntcam root)
	s, n := use(filepath.Join(d, "sub"))
	if !s.Installed || !strings.Contains(n, "ignoring the build pointers in "+filepath.Join(d, "build")) || !strings.Contains(n, "not a stuntcam checkout") {
		t.Fatalf("%+v %q", s, n)
	}

	// a stuntcam root whose pointers leave home.Builds()
	d = t.TempDir()
	fakeCheckout(t, d, true)
	o := t.TempDir()
	write(t, filepath.Join(o, "x", LoaderName), "L")
	write(t, filepath.Join(o, "x", EngineName), "E")
	write(t, filepath.Join(d, "build", "current_loader.txt"), filepath.Join(o, "x", LoaderName)+"\n")
	write(t, filepath.Join(d, "build", "current_dylib.txt"), filepath.Join(o, "x", EngineName)+"\n")
	if s, n = use(d); !s.Installed || !strings.Contains(n, "not in a content-hash dir under") {
		t.Fatalf("%+v %q", s, n)
	}

	// a stuntcam root whose staged build doesn't hash to its name
	d = t.TempDir()
	pair := fakeCheckout(t, d, true)
	write(t, filepath.Join(home.Builds(), pair, EngineName), "E2 changed")
	if s, n = use(d); !s.Installed || !strings.Contains(n, "engine+loader hashes to") {
		t.Fatalf("%+v %q", s, n)
	}

	// a stuntcam root whose build is missing
	d = t.TempDir()
	pair = fakeCheckout(t, d, true)
	os.RemoveAll(filepath.Join(home.Builds(), pair))
	if s, n = use(d); !s.Installed || !strings.Contains(n, "ignoring") {
		t.Fatalf("%+v %q", s, n)
	}

	// a real checkout with a good build, opted in: used, and said so, with what it uses
	d = good
	pair = fakeCheckout(t, d, true)
	if pair != goodPair {
		t.Fatal(pair)
	}
	if s, n = use(filepath.Join(d, "engine")); s.Installed || s.BuildID() != pair || !strings.Contains(n, "using the stuntcam checkout in "+d+" ("+UseCheckoutEnv+"=1)") ||
		!strings.Contains(n, "canary "+filepath.Join(d, "canary", "build", "m10canary.app")) || !strings.Contains(n, "profile "+filepath.Join(d, "engine", "profiles")) ||
		!strings.Contains(n, "restore rules "+filepath.Join(d, "restore-rules")) {
		t.Fatalf("%+v %q", s, n)
	}
	t.Setenv(UseCheckoutEnv, "")

	// --build / STUNTCAM_BUILD from an installed binary: said too. A staged $SIMCAM_HOME/builds/<hash> has no canary,
	// profile or rules of its own: the installed package's are used (review NIT), and named
	notes = nil
	noteSeen = map[string]bool{}
	s, err = find(filepath.Join(home.Builds(), pair), prefix, t.TempDir())
	if err != nil || s.BuildID() != pair || len(notes) != 1 || !strings.Contains(notes[0], "--build or STUNTCAM_BUILD") {
		t.Fatalf("%+v %v %q", s, err, notes)
	}
	if s.Canary != pkg.Canary || s.Profile != pkg.Profile || s.Image != pkg.Image || s.Rules != pkg.Rules ||
		!strings.Contains(notes[0], "canary "+pkg.Canary) || !strings.Contains(notes[0], "engine+loader "+pair) {
		t.Fatalf("--build from an installed binary: %+v %q", s, notes)
	}
	// after a first up/verify/exec, $SIMCAM_HOME/restore-rules (the staging cache) and $SIMCAM_HOME/canary exist: still
	// the package's (review M1)
	if _, err := StageRules(pkg.Rules); err != nil {
		t.Fatal(err)
	}
	os.MkdirAll(filepath.Join(home.Root(), "canary", "m10canary.app"), 0o755)
	if s, err = find(filepath.Join(home.Builds(), pair), prefix, t.TempDir()); err != nil || s.Rules != pkg.Rules || s.Canary != pkg.Canary {
		t.Fatalf("staged build after a first run: %+v %v", s, err)
	}
	// STUNTCAM_CANARY / STUNTCAM_PROFILE still win
	t.Setenv("STUNTCAM_CANARY", filepath.Join(d, "c.app"))
	t.Setenv("STUNTCAM_PROFILE", filepath.Join(d, "p.plist"))
	write(t, filepath.Join(d, "p.plist"), "p")
	if s, err = find(filepath.Join(home.Builds(), pair), prefix, t.TempDir()); err != nil || s.Canary != filepath.Join(d, "c.app") || s.Profile != filepath.Join(d, "p.plist") {
		t.Fatalf("%+v %v", s, err)
	}
	t.Setenv("STUNTCAM_CANARY", "")
	t.Setenv("STUNTCAM_PROFILE", "")

	// a binary that isn't installed keeps the old order and says nothing
	notes = nil
	noteSeen = map[string]bool{}
	if s, err := find("", "", d); err != nil || s.BuildID() != pair || len(notes) != 0 {
		t.Fatalf("%+v %v %q", s, err, notes)
	}
}

// review (1): several stuntcams unpacking the same package at once (two commands right after an install) all
// succeed, and each one finds the canary complete and executable the moment Installed returns: from a fresh home, over
// a canary that lost its exec bit, and over one with changed bytes.
func TestInstalledConcurrentUnpack(t *testing.T) {
	prefix := t.TempDir()
	installedTree(t, prefix, false)
	for round, spoil := range []func(canary string){
		nil,
		func(c string) { os.Chmod(filepath.Join(c, "m10canary"), 0o644) },
		func(c string) { write(t, filepath.Join(c, "m10canary"), "x") },
		func(c string) { os.RemoveAll(c) },
	} {
		var canary string
		if spoil != nil {
			s, err := Installed(prefix)
			if err != nil {
				t.Fatal(err)
			}
			canary = s.Canary
			spoil(canary)
		}
		const n = 16
		start := make(chan struct{})
		errs := make(chan error, n)
		for i := 0; i < n; i++ {
			go func() {
				<-start
				s, err := Installed(prefix)
				if err == nil {
					fi, serr := os.Stat(filepath.Join(s.Canary, "m10canary"))
					b, _ := os.ReadFile(filepath.Join(s.Canary, "m10canary"))
					if serr != nil || fi.Mode().Perm() != 0o755 || string(b) != "c" {
						err = fmt.Errorf("canary right after Installed: %v %v %q", fi, serr, b)
					}
				}
				errs <- err
			}()
		}
		close(start)
		for i := 0; i < n; i++ {
			if err := <-errs; err != nil {
				t.Errorf("round %d: %v", round, err)
			}
		}
		if left, _ := filepath.Glob(filepath.Join(home.Canary(), ".[so]*")); len(left) != 0 {
			t.Errorf("round %d: temp dirs left: %v", round, left)
		}
	}
}

// The same with separate processes (flock is per open file, across processes): the probe binary installed as the
// package's bin/, started together from a fresh home, several times.
func TestInstalledConcurrentProcesses(t *testing.T) {
	prefix := t.TempDir()
	installedTree(t, prefix, false)
	probe := filepath.Join(prefix, "bin", "probe")
	if err := exec.Command("go", "build", "-o", probe, "./testdata/probe").Run(); err != nil {
		t.Fatalf("go build: %v", err)
	}
	for round := 0; round < 5; round++ {
		t.Setenv("SIMCAM_HOME", filepath.Join(t.TempDir(), "home"))
		goFile := filepath.Join(t.TempDir(), "go")
		const n = 8
		cmds := make([]*exec.Cmd, n)
		outs := make([]*bytes.Buffer, n)
		for i := range cmds {
			outs[i] = &bytes.Buffer{}
			cmds[i] = exec.Command(probe, "here")
			cmds[i].Env = append(os.Environ(), "PROBE_GO="+goFile)
			cmds[i].Stdout, cmds[i].Stderr = outs[i], outs[i]
			if err := cmds[i].Start(); err != nil {
				t.Fatal(err)
			}
		}
		write(t, goFile, "")
		for i, c := range cmds {
			if err := c.Wait(); err != nil || !strings.HasPrefix(outs[i].String(), "ok ") {
				t.Errorf("round %d process %d: %v %s", round, i, err, outs[i])
			}
		}
	}
}

// review (2): a packaged binary carries the sha256 of manifest.txt and payload.tar; an installed share/ that was
// replaced consistently (new payload, manifest re-written to match) is refused. A dev build (no values) accepts it.
func TestInstalledAnchoredToBinary(t *testing.T) {
	defer func(m, p string) { packagedManifest, packagedPayload = m, p }(packagedManifest, packagedPayload)
	prefix := t.TempDir()
	installedTree(t, prefix, false)
	share := filepath.Join(prefix, "share", "stuntcam")
	sum := func(p string) string {
		h, err := sha256File(p)
		if err != nil {
			t.Fatal(err)
		}
		return h
	}
	man, pay := sum(filepath.Join(share, ManifestName)), sum(filepath.Join(share, PayloadName))

	packagedManifest, packagedPayload = man, pay
	if ok, h := Anchored(); !ok || h != man {
		t.Fatal(ok, h)
	}
	if _, err := Installed(prefix); err != nil {
		t.Fatalf("the packaged files: %v", err)
	}

	// re-make the payload with another engine and re-write the manifest to match: internally consistent
	os.RemoveAll(share)
	h := func(b string) string { s := sha1.Sum([]byte(b)); return hex.EncodeToString(s[:])[:12] }
	writeInstalled(t, prefix, [][2]string{
		{"builds/" + h("evilloader") + "/" + EngineName, "evil"}, {"builds/" + h("evilloader") + "/" + LoaderName, "loader"},
		{"stub/" + h("stub") + "/" + StubName, "stub"}, {"canary/m10canary.app/m10canary", "c"},
	}, map[string]string{"profiles/cfg-m10.plist": "p", "media/qr.png": "i"})
	packagedManifest, packagedPayload = "", ""
	if _, err := Installed(prefix); err != nil {
		t.Fatalf("a dev build checks consistency only: %v", err)
	}
	os.RemoveAll(home.Builds())
	packagedManifest, packagedPayload = man, pay
	_, err := Installed(prefix)
	if err == nil || !strings.Contains(err.Error(), "doesn't match its binary: manifest.txt has sha256") || !strings.Contains(err.Error(), "packaged with "+man[:12]) {
		t.Fatalf("%v", err)
	}
	// only the payload's hash embedded: the manifest's payload line is checked against it
	packagedManifest = ""
	if _, err = Installed(prefix); err == nil || !strings.Contains(err.Error(), "manifest.txt lists payload.tar with sha256") {
		t.Fatalf("%v", err)
	}
	if exists(filepath.Join(home.Builds(), h("evilloader"))) {
		t.Fatal("unpacked a payload the binary wasn't packaged with")
	}
}

// review (5): Finder's .DS_Store anywhere under share/stuntcam is ignored, and only that name.
func TestInstalledIgnoresDSStore(t *testing.T) {
	prefix := t.TempDir()
	installedTree(t, prefix, true)
	share := filepath.Join(prefix, "share", "stuntcam")
	for _, d := range []string{"", "media", "restore-rules", "verdicts"} {
		write(t, filepath.Join(share, d, ".DS_Store"), "\x00\x00\x00\x01Bud1")
	}
	if _, err := Installed(prefix); err != nil {
		t.Fatalf(".DS_Store: %v", err)
	}
	for _, name := range []string{"._qr.png", ".DS_Store2", "DS_Store"} {
		p := filepath.Join(share, "media", name)
		write(t, p, "x")
		if _, err := Installed(prefix); err == nil || !strings.Contains(err.Error(), "media/"+name+" is not in manifest.txt") {
			t.Errorf("%s: %v", name, err)
		}
		os.Remove(p)
	}
	os.Remove(filepath.Join(share, "media", ".DS_Store"))
	os.Symlink("/etc/hosts", filepath.Join(share, "media", ".DS_Store"))
	if _, err := Installed(prefix); err == nil || !strings.Contains(err.Error(), "media/.DS_Store is not a regular file") {
		t.Errorf("a .DS_Store symlink: %v", err)
	}
}

// review (7): InstalledHere checks the binary's own package wherever it runs, even inside a stuntcam checkout.
func TestInstalledHereIgnoresWorkingDirectory(t *testing.T) {
	prefix := t.TempDir()
	pair, _, _ := installedTree(t, prefix, false)
	probe := filepath.Join(prefix, "bin", "probe")
	if err := exec.Command("go", "build", "-o", probe, "./testdata/probe").Run(); err != nil {
		t.Fatalf("go build: %v", err)
	}
	d := t.TempDir()
	fakeCheckout(t, d, true)
	for _, env := range [][]string{nil, {UseCheckoutEnv + "=1"}, {"STUNTCAM_BUILD=" + d}} {
		cmd := exec.Command(probe, "here")
		cmd.Dir = d
		cmd.Env = append(os.Environ(), env...)
		if o, err := cmd.CombinedOutput(); err != nil || strings.TrimSpace(string(o)) != "ok "+pair {
			t.Errorf("%v: %s %v", env, o, err)
		}
	}
	if _, err := InstalledHere(); err == nil || !strings.Contains(err.Error(), "not an installed stuntcam") {
		t.Errorf("the test binary is not installed: %v", err)
	}
}
