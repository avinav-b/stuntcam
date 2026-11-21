// SPDX-License-Identifier: Apache-2.0
package artifacts

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// helper/build.sh's <sha>: sha1 over "<path> <sha1>\n" lines in byte order, first 12 hex digits
func TestHelperBundleSHA1MatchesBuildSh(t *testing.T) {
	// printf 'Contents/Info.plist %s\nContents/MacOS/stuntcam-camera %s\n' $(printf p | shasum | cut -c1-40) $(printf x | shasum | cut -c1-40) | shasum
	got := helperBundleSHA1(map[string][]byte{"Contents/MacOS/stuntcam-camera": []byte("x"), "Contents/Info.plist": []byte("p")})
	if got != "51b3e2cb3c7a" {
		t.Fatal(got)
	}
}

func helperMembers(files map[string]string) ([][2]string, string) {
	data := map[string][]byte{}
	for r, c := range files {
		data[r] = []byte(c)
	}
	sha := helperBundleSHA1(data)
	var ms [][2]string
	for r, c := range files {
		ms = append(ms, [2]string{"helper/" + sha + "/" + HelperApp + "/" + r, c})
	}
	return ms, sha
}

func TestInstalledUnpacksHelper(t *testing.T) {
	prefix := t.TempDir()
	pair, stub, hook := installedTree(t, prefix, false)
	base := [][2]string{
		{"builds/" + pair + "/" + EngineName, "engine"}, {"builds/" + pair + "/" + LoaderName, "loader"},
		{"stub/" + stub + "/" + StubName, "stub"}, {"hook/" + hook + "/" + HookName, "hook"},
		{"canary/m10canary.app/m10canary", "c"},
	}
	loose := map[string]string{"profiles/cfg-m10.plist": "p", "media/qr.png": "i"}
	hm, sha := helperMembers(map[string]string{HelperExe: "x", "Contents/Info.plist": "p", "Contents/_CodeSignature/CodeResources": "r"})
	os.RemoveAll(filepath.Join(prefix, "share"))
	writeInstalled(t, prefix, append(append([][2]string{}, base...), hm...), loose)
	s, err := Installed(prefix)
	want := filepath.Join(home.Root(), "helper", sha, HelperApp)
	if err != nil || s.Helper != want || s.HelperSHA() != sha {
		t.Fatalf("%q %v", s.Helper, err)
	}
	if fi, err := os.Stat(filepath.Join(want, HelperExe)); err != nil || fi.Mode().Perm() != 0o755 {
		t.Fatalf("exec bit: %v %v", fi, err)
	}
	if fi, _ := os.Stat(filepath.Join(want, "Contents/Info.plist")); fi.Mode().Perm() != 0o644 {
		t.Fatal(fi.Mode())
	}
	// a changed copy is put back
	write(t, filepath.Join(want, HelperExe), "tampered")
	if _, err := Installed(prefix); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(filepath.Join(want, HelperExe)); string(b) != "x" {
		t.Fatalf("%q", b)
	}

	// a dir name that isn't the bundle's hash, a helper without its executable, two helpers: refused
	for name, ms := range map[string][][2]string{
		"hash":  {{"helper/0123456789ab/" + HelperApp + "/" + HelperExe, "x"}},
		"exe":   func() [][2]string { m, _ := helperMembers(map[string]string{"Contents/Info.plist": "p"}); return m }(),
		"two":   append(append([][2]string{}, hm...), func() [][2]string { m, _ := helperMembers(map[string]string{HelperExe: "y"}); return m }()...),
		"other": {{"helper/" + sha + "/Other.app/" + HelperExe, "x"}},
	} {
		os.RemoveAll(filepath.Join(prefix, "share"))
		writeInstalled(t, prefix, append(append([][2]string{}, base...), ms...), loose)
		if _, err := Installed(prefix); err == nil || !strings.Contains(err.Error(), "changed after packaging") {
			t.Errorf("%s: %v", name, err)
		}
	}
}

func TestCheckoutFindsHelper(t *testing.T) {
	d := t.TempDir()
	for _, f := range []string{"b/libsimcamloader.dylib", "b/libsimcam.dylib", "s/libsimcamstub.dylib"} {
		write(t, filepath.Join(d, f), "x")
	}
	write(t, filepath.Join(d, "build", "current_loader.txt"), filepath.Join(d, "b", LoaderName)+"\n")
	write(t, filepath.Join(d, "build", "current_stub.txt"), filepath.Join(d, "s", StubName)+"\n")
	if s, err := checkoutFrom(d); err != nil || s.Helper != "" {
		t.Fatalf("%q %v", s.Helper, err)
	}
	app := "/h/helper/0123456789ab/" + HelperApp
	write(t, filepath.Join(d, "build", "current_helper.txt"), app+"\n")
	if s, err := checkoutFrom(d); err != nil || s.Helper != app || s.HelperSHA() != "0123456789ab" {
		t.Fatalf("%q %v", s.Helper, err)
	}
}
