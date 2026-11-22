// SPDX-License-Identifier: Apache-2.0
package home

import (
	"os"
	"path/filepath"
	"testing"
)

func TestParseCurrent(t *testing.T) {
	tests := []struct {
		name, line string
		ok         bool
	}{
		{"loader with newline", "1 /a/b/libsimcamloader.dylib\n", true},
		{"short path", "1 /x", true},
		// the stub refuses the same lines
		{"empty", "", false},
		{"no proto", "abc def", false},
		{"proto 0", "0 /x", false},
		{"relative path", "1 relative/x", false},
		{"no path", "1", false},
		{"negative proto", "-1 /x", false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			p, l, err := ParseCurrent(tt.line)
			if !tt.ok {
				if err == nil {
					t.Errorf("%q accepted", tt.line)
				}
				return
			}
			if err != nil || p != 1 || l[0] != '/' {
				t.Errorf("%q: %d %q %v", tt.line, p, l, err)
			}
		})
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
	tests := []struct {
		name, path string
		want       bool
	}{
		{"under Documents", filepath.Join(d, "Documents", "Github", "app"), true},
		{"Desktop", filepath.Join(d, "Desktop"), true},
		{"a name starting with Documents", filepath.Join(d, "DocumentsX"), false},
		{"Application Support", filepath.Join(d, "Library", "Application Support", "stuntcam"), false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := Protected(tt.path); got != tt.want {
				t.Errorf("%s: got %v", tt.path, got)
			}
		})
	}
}

// a rename that fails (the target is a directory) leaves no temp file behind
func TestWriteAtomicRenameFailure(t *testing.T) {
	d := t.TempDir()
	target := filepath.Join(d, "f")
	if err := os.Mkdir(filepath.Join(target), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(target, "x"), nil, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := WriteAtomic(target, []byte("data")); err == nil {
		t.Fatal("rename onto a non-empty directory succeeded")
	}
	if es, _ := os.ReadDir(d); len(es) != 1 {
		t.Fatalf("left behind: %v", es)
	}
}
