// SPDX-License-Identifier: Apache-2.0
package arm

import (
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// Tests for the engine settings' stuntcam 1.1.0 names (LegacyNames): what Vars sets, and launchd arms recorded by 1.1.0.

// the names stuntcam 1.1.0's Vars set for the engine (testdata/arm-1.1.0-launchd.json records an `up --log` arm)
var names110 = []string{"M2_FAKE", "M2_SESSION_HOOK", "M2_NOLOGDICT", "M2_PLIST", "M5_IMAGE", "M5_LOG"}

func fullSet() artifacts.Set {
	s := artifacts.Set{Stub: "/h/builds/s1/libsimcamstub.dylib", Loader: "/h/builds/b1/libsimcamloader.dylib",
		Engine: "/h/builds/b1/libsimcam.dylib"}
	s.Profile, s.Image = "/h/data/p1/profile.plist", "/h/data/i1/qr.png"
	return s
}

// value of k in a Vars list, and whether it is there
func lookup(vars []string, k string) (string, bool) {
	for _, kv := range vars {
		if n, v, _ := strings.Cut(kv, "="); n == k {
			return v, true
		}
	}
	return "", false
}

func TestLegacyNamesAre110Names(t *testing.T) {
	var old []string
	for _, o := range LegacyNames {
		old = append(old, o)
	}
	slices.Sort(old)
	want := slices.Clone(names110)
	slices.Sort(want)
	if !slices.Equal(old, want) {
		t.Fatalf("LegacyNames values %v, want 1.1.0's %v", old, want)
	}
}

func TestVarsEmitsLegacyNames(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	oldStyle := regexp.MustCompile(`^M[0-9]+_`) // the 1.1.0 naming scheme
	for _, stubLog := range []bool{false, true} {
		vars := Vars("U", fullSet(), Options{StubLog: stubLog, Extra: map[string]string{"SIMCAM_SOURCE": "scene"}})
		for i, kv := range vars {
			k, v, _ := strings.Cut(kv, "=")
			if old, ok := LegacyNames[k]; ok {
				if i+1 >= len(vars) || vars[i+1] != old+"="+v {
					t.Errorf("stubLog=%v: %s is not followed by %s=%s: %v", stubLog, kv, old, v, vars)
				}
			}
			if oldStyle.MatchString(k) && !slices.Contains(names110, k) {
				t.Errorf("stubLog=%v: %s is set, an M*_ name that is not a 1.1.0 name", stubLog, k)
			}
		}
		for k, old := range LegacyNames {
			nv, nok := lookup(vars, k)
			ov, ook := lookup(vars, old)
			if nok != ook || nv != ov {
				t.Errorf("stubLog=%v: %s=%q (%v) but %s=%q (%v)", stubLog, k, nv, nok, old, ov, ook)
			}
			if want := k != "SIMCAM_ENGINE_LOG" || stubLog; nok != want {
				t.Errorf("stubLog=%v: %s set %v, want %v", stubLog, k, nok, want)
			}
		}
		if v, _ := lookup(vars, "M2_PLIST"); v != fullSet().Profile {
			t.Errorf("M2_PLIST=%q", v)
		}
		if n := len(vars); n != len(names(vars)) || len(union(names(vars), nil)) != n {
			t.Errorf("a name is set twice: %v", vars)
		}
	}
}

// a sim armed in launchd mode by 1.1.0: its record and its launchd environment
func armedBy110(t *testing.T) *fakeSim {
	t.Helper()
	t.Setenv("SIMCAM_HOME", t.TempDir())
	b, err := os.ReadFile(filepath.Join("testdata", "arm-1.1.0-launchd.json"))
	if err != nil {
		t.Fatal(err)
	}
	os.MkdirAll(home.Sim("U"), 0o755)
	if err := os.WriteFile(recordPath("U"), b, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := home.WriteCurrent("U", "/h/builds/b0/libsimcamloader.dylib"); err != nil {
		t.Fatal(err)
	}
	r, err := ReadRecord("U")
	if err != nil || r.Mode != Launchd || r.Build != "b0" || len(r.Vars) != 12 {
		t.Fatalf("1.1.0 record reads as %+v, %v", r, err)
	}
	f := &fakeSim{env: map[string]string{
		"DYLD_INSERT_LIBRARIES": "/t3/serve-sim.dylib:/h/builds/s0/libsimcamstub.dylib",
		"PATH":                  "/usr/bin",
	}}
	for _, k := range r.Vars[1:] {
		f.env[k] = "old-" + k
	}
	return f
}

func TestDisarm110LaunchdRecord(t *testing.T) {
	f := armedBy110(t)
	if err := Disarm(f, "U", true); err != nil {
		t.Fatal(err)
	}
	want := map[string]string{"DYLD_INSERT_LIBRARIES": "/t3/serve-sim.dylib", "PATH": "/usr/bin"}
	if len(f.env) != len(want) || f.env["DYLD_INSERT_LIBRARIES"] != want["DYLD_INSERT_LIBRARIES"] || f.env["PATH"] == "" {
		t.Fatalf("after down: %v, want %v (every recorded name unset, other tools' entries kept)", f.env, want)
	}
	if _, err := os.Stat(recordPath("U")); !os.IsNotExist(err) {
		t.Fatal("record not removed")
	}
}

func TestArmLaunchdOver110Record(t *testing.T) {
	f := armedBy110(t)
	prev, _ := ReadRecord("U")
	s := fullSet()
	vars := Vars("U", s, Options{StubLog: true})
	if err := ArmLaunchd(f, "U", s, vars); err != nil {
		t.Fatal(err)
	}
	// no setting is ever unset on the way: apps launched during the re-arm keep one name of each
	for k, old := range LegacyNames {
		if slices.Contains(f.unset, k) || slices.Contains(f.unset, old) {
			t.Errorf("the re-arm unset %s or %s on the way (unset calls: %v)", k, old, f.unset)
		}
	}
	for k, old := range LegacyNames {
		want, _ := lookup(vars, k)
		if f.env[k] != want || f.env[old] != want {
			t.Errorf("%s=%q %s=%q, want both %q", k, f.env[k], old, f.env[old], want)
		}
	}
	if f.env["DYLD_INSERT_LIBRARIES"] != "/t3/serve-sim.dylib:"+s.Stub {
		t.Errorf("DYLD_INSERT_LIBRARIES=%q", f.env["DYLD_INSERT_LIBRARIES"])
	}
	r, _ := ReadRecord("U")
	if want := union(prev.Vars, names(vars)); !slices.Equal(r.Vars, want) || r.Build != "b1" {
		t.Fatalf("record %+v, want vars %v", r, want)
	}
	if err := Disarm(f, "U", true); err != nil {
		t.Fatal(err)
	}
	if len(f.env) != 2 || f.env["DYLD_INSERT_LIBRARIES"] != "/t3/serve-sim.dylib" {
		t.Fatalf("after down: %v", f.env)
	}
}
