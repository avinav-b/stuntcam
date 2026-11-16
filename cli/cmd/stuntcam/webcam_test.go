// SPDX-License-Identifier: Apache-2.0
package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/webcam"
)

func TestWebcamSpec(t *testing.T) {
	for spec, want := range map[string]string{"webcam": "", "webcam:OBSBOT": "OBSBOT", "webcam:synthetic": "synthetic"} {
		if n, ok := webcamSpec(spec); !ok || n != want {
			t.Error(spec, n, ok)
		}
	}
	for _, spec := range []string{"scene", "webcam:", "webcams", "video:webcam"} {
		if _, ok := webcamSpec(spec); ok {
			t.Error(spec)
		}
	}
}

// a helper that lists two cameras and starts authorized; records each open
func fakeWebcam(t *testing.T) (*webcam.Env, artifacts.Set, *[][]string) {
	h := t.TempDir()
	app := filepath.Join(h, "helper", "aaaaaaaaaaaa", webcam.AppName)
	os.MkdirAll(filepath.Join(app, "Contents", "MacOS"), 0o755)
	os.WriteFile(filepath.Join(app, webcam.ExeRel), []byte("x"), 0o755)
	var opens [][]string
	alive := map[int]bool{}
	now := time.Unix(1794790000, 0)
	e := &webcam.Env{Home: h, Rings: []int{1}, Uid: 501, Verify: func(string) error { return nil },
		Alive: func(p int) bool { return alive[p] }, Term: func(p int) error { alive[p] = false; return nil },
		IsHelper: func(int) bool { return true }, ShmOwner: func(string) (int, error) { return -1, os.ErrNotExist },
		Now: func() time.Time { return now }, Sleep: func(d time.Duration) { now = now.Add(d) },
		Out: &strings.Builder{}, StartWait: 5 * time.Second, PermWait: 60 * time.Second, StopWait: 5 * time.Second, Poll: 100 * time.Millisecond}
	e.Open = func(_ string, args []string) error {
		opens = append(opens, args)
		if args[2] == "--list" {
			return os.WriteFile(args[3], []byte("stuntcam-cameras 1\nBUILTIN\t1\t0\tT\tMacBook Pro Camera\nEXT\t0\t0\tT\tOBSBOT Meet\nPHONE\t0\t1\tT\tPhone\n"), 0o600)
		}
		id := webcam.SyntheticID
		if args[2] == "--camera" {
			id = args[3]
		}
		pid := 100 + len(opens)
		alive[pid] = true
		os.MkdirAll(webcam.Dir(h), 0o700)
		return os.WriteFile(webcam.PidPath(h, webcam.CamHash(h, id)), []byte(fmt.Sprintf("stuntcam-webcam 1\npid=%d\nsha=aaaaaaaaaaaa\nrings=1\nauth=authorized\nstate=idle\ncamera=%s\nstarted=%d\n", pid, id, now.Unix())), 0o600)
	}
	return e, artifacts.Set{Helper: app}, &opens
}

func TestStartWebcam(t *testing.T) {
	e, set, opens := fakeWebcam(t)
	id, err := startWebcam(e, set, "", "", "")
	if err != nil || id != "BUILTIN" || len(*opens) != 2 {
		t.Fatal(id, err, *opens)
	}
	if b, _ := os.ReadFile(webcam.DefaultPath(e.Home)); string(b) != "BUILTIN\n" {
		t.Fatalf("default file %q", b)
	}
	if id, err := startWebcam(e, set, "obsbot", "", ""); err != nil || id != "EXT" {
		t.Fatal(id, err)
	}
	if id, err := startWebcam(e, set, "", "OBSBOT Meet", "EXT"); err != nil || id != "EXT" {
		t.Fatal("--front and --back naming the same camera:", id, err)
	}
	if _, err := startWebcam(e, set, "", "EXT", "BUILTIN"); err == nil || !strings.Contains(err.Error(), "one camera to both positions") {
		t.Fatal(err)
	}
	if _, err := startWebcam(e, set, "nope", "", ""); err == nil || !strings.Contains(err.Error(), "BUILTIN") {
		t.Fatal(err)
	}
	// the synthetic pattern needs no camera list and never becomes the default
	n := len(*opens)
	if id, err := startWebcam(e, set, "synthetic", "", ""); err != nil || id != webcam.SyntheticID || len(*opens) != n+1 || (*opens)[n][2] != "--synthetic" {
		t.Fatal(id, err, *opens)
	}
	if b, _ := os.ReadFile(webcam.DefaultPath(e.Home)); string(b) != "EXT\n" {
		t.Fatalf("default file %q", b)
	}
	if _, err := startWebcam(e, artifacts.Set{}, "", "", ""); err == nil || !strings.Contains(err.Error(), "no camera helper") {
		t.Fatal(err)
	}
}

func TestControlSource(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if controlSource("U") != "" {
		t.Fatal()
	}
	os.MkdirAll(filepath.Join(os.Getenv("SIMCAM_HOME"), "sims", "U"), 0o755)
	os.WriteFile(filepath.Join(os.Getenv("SIMCAM_HOME"), "sims", "U", "control"), []byte("orientation=auto\nsource=webcam:EXT\n"), 0o644)
	if controlSource("U") != "webcam:EXT" {
		t.Fatal(controlSource("U"))
	}
}
