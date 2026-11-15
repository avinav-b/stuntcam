// SPDX-License-Identifier: Apache-2.0
package home

import (
	"os"
	"path/filepath"
	"testing"
)

func TestParseCurrent(t *testing.T) {
	ok := []string{"1 /a/b/libsimcamloader.dylib\n", "1 /x"}
	for _, s := range ok {
		if p, l, err := ParseCurrent(s); err != nil || p != 1 || l[0] != '/' {
			t.Errorf("%q: %d %q %v", s, p, l, err)
		}
	}
	bad := []string{"", "abc def", "0 /x", "1 relative/x", "1", "-1 /x"} // the stub refuses the same lines
	for _, s := range bad {
		if _, _, err := ParseCurrent(s); err == nil {
			t.Errorf("%q accepted", s)
		}
	}
}

func TestWriteReadCurrent(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if err := WriteCurrent("U1", "rel/loader"); err == nil {
		t.Fatal("relative loader accepted")
	}
	if err := WriteCurrent("U1", "/b/h/libsimcamloader.dylib"); err != nil {
		t.Fatal(err)
	}
	p, l, err := ReadCurrent("U1")
	if err != nil || p != Proto || l != "/b/h/libsimcamloader.dylib" {
		t.Fatalf("%d %q %v", p, l, err)
	}
	if ents, _ := os.ReadDir(Sim("U1")); len(ents) != 1 {
		t.Fatalf("temp file left behind: %v", ents)
	}
}

func TestProtected(t *testing.T) {
	d, _ := os.UserHomeDir()
	cases := map[string]bool{
		filepath.Join(d, "Documents", "Github", "app"):                 true,
		filepath.Join(d, "Desktop"):                                    true,
		filepath.Join(d, "DocumentsX"):                                 false,
		filepath.Join(d, "Library", "Application Support", "stuntcam"): false,
	}
	for p, want := range cases {
		if got := Protected(p); got != want {
			t.Errorf("%s: got %v", p, got)
		}
	}
}
