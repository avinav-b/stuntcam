// SPDX-License-Identifier: Apache-2.0
package state

import (
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

func TestAnalyzeMatchesEngineRules(t *testing.T) {
	now := time.Unix(1_800_000_000, 0)
	ts := func(ago time.Duration) string { return itoa(now.Add(-ago).Unix()) }
	lines := []string{
		ts(time.Hour) + " 1 activate - B1 traced=0",
		ts(time.Hour) + " 1 crash photo B1 signal=11",
		ts(50*time.Minute) + " 2 activate - B1 traced=0",
		ts(50*time.Minute) + " 2 crash photo B1 signal=11",
		ts(40*time.Minute) + " 3 activate - B1 traced=0",
		ts(40*time.Minute) + " 3 crash-other - B1 signal=6", // never counted
		ts(40*time.Minute) + " 3 hang session OLD n=1",      // other build: not counted for B1
	}
	a := Analyze("dev.x", Parse(lines), "", now)
	if got := a.LatchedFeatures(); len(got) != 1 || got[0] != "photo" || a.Activations != 3 {
		t.Fatalf("%+v", a)
	}
	// 24 h decay
	old := []string{ts(25*time.Hour) + " 1 crash photo B1 x", ts(25*time.Hour) + " 2 crash photo B1 x", ts(time.Minute) + " 3 activate - B1 traced=0"}
	if got := Analyze("x", Parse(old), "", now).LatchedFeatures(); len(got) != 0 {
		t.Fatalf("aged-out crashes latched: %v", got)
	}
	// 20-activation decay
	var many []string
	many = append(many, ts(time.Hour)+" 1 crash photo B1 x", ts(time.Hour)+" 2 crash photo B1 x")
	for i := 3; i <= 25; i++ {
		many = append(many, ts(time.Minute)+" "+itoa(int64(i))+" activate - B1 traced=0")
	}
	if got := Analyze("x", Parse(many), "", now).LatchedFeatures(); len(got) != 0 {
		t.Fatalf("crashes 23 activations ago latched: %v", got)
	}
	// clear forgets
	cleared := append(append([]string{}, lines...), ts(time.Minute)+" 0 clear - - cli")
	if got := Analyze("x", Parse(cleared), "B1", now).LatchedFeatures(); len(got) != 0 {
		t.Fatalf("after clear: %v", got)
	}
}

func TestUnknownDebugEnd(t *testing.T) {
	now := time.Now()
	l := []string{itoa(now.Unix()) + " 4 activate - B1 traced=1", itoa(now.Unix()) + " 4 op-begin session B1 slot=0"}
	if a := Analyze("x", Parse(l), "", now); a.UnknownDebugEnd != "session" || len(a.LatchedFeatures()) != 0 {
		t.Fatalf("%+v", a)
	}
}

func TestClearAppends(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if err := Clear("U", "dev.x", "test"); err != nil {
		t.Fatal(err)
	}
	b, _ := os.ReadFile(EventsFile("U", "dev.x"))
	if e := Parse([]string{string(b[:len(b)-1])}); len(e) != 1 || e[0].Kind != "clear" {
		t.Fatalf("%q", b)
	}
}

func TestMarkersAndClear(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if featureOf("activate") != "activate" || featureOf("bogus") != "engine" {
		t.Fatal("featureOf")
	}
	dir := filepath.Join(home.StateDir("U"), "dev.x")
	os.MkdirAll(dir, 0o755)
	mk := filepath.Join(dir, "latched-abc")
	os.WriteFile(mk, []byte("session photo\n"), 0o644)
	until := time.Now().Add(23 * time.Hour).Truncate(time.Second)
	os.Chtimes(mk, until, until)
	pf := filepath.Join(home.StateDir("U"), "preflight-fail-abc")
	os.WriteFile(pf, []byte("x\n"), 0o644)
	m := Markers("U", "dev.x")
	if len(m) != 1 || m[0].Build != "abc" || m[0].Features != "session photo" || !m[0].Until.Equal(until) || !m[0].Valid(time.Now()) {
		t.Fatalf("%+v", m)
	}
	if err := Clear("U", "dev.x", "test"); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(mk); err == nil {
		t.Fatal("marker kept")
	}
	if _, err := os.Stat(pf); err == nil {
		t.Fatal("preflight-fail kept")
	}
}

func TestMotionFeature(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	if featureOf("motion") != "motion" || ClosesCamera("motion") || !ClosesCamera("activate") {
		t.Fatal("motion feature")
	}
	if e := Effect([]string{"motion"}); e != "stock CoreMotion (the camera is unaffected)" {
		t.Fatal(e)
	}
	if e := Effect([]string{"motion", "photo"}); e != "no camera and stock CoreMotion" {
		t.Fatal(e)
	}
	now := time.Unix(1_800_000_000, 0)
	l := []string{itoa(now.Unix()-60) + " 1 activate - B1 traced=0", itoa(now.Unix()-60) + " 1 crash motion B1 signal=11",
		itoa(now.Unix()) + " 2 activate - B1 traced=0", itoa(now.Unix()) + " 2 crash motion B1 signal=11"}
	if got := Analyze("x", Parse(l), "", now).LatchedFeatures(); len(got) != 1 || got[0] != "motion" {
		t.Fatalf("%v", got)
	}
	dir := filepath.Join(home.StateDir("U"), "dev.x")
	os.MkdirAll(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "latched-motion-abc"), []byte("motion\n"), 0o644)
	os.WriteFile(filepath.Join(dir, "latched-abc"), []byte("activate\n"), 0o644)
	pf := filepath.Join(home.StateDir("U"), "preflight-fail-abc")
	os.WriteFile(pf, []byte("preflight failed\n"), 0o644)
	until := time.Now().Add(-time.Hour).Truncate(time.Second)
	os.Chtimes(pf, until, until)
	ms := Markers("U", "dev.x")
	if len(ms) != 2 {
		t.Fatalf("%+v", ms)
	}
	for _, m := range ms {
		if m.Build != "abc" || (m.Motion != (m.Features == "motion")) || (m.Motion && m.Name() != "latched-motion-abc") || (!m.Motion && m.Name() != "latched-abc") {
			t.Fatalf("%+v", m)
		}
	}
	if p := PreflightFails("U"); len(p) != 1 || p[0].Build != "abc" || p[0].What != "preflight failed" || p[0].Valid(time.Now()) {
		t.Fatalf("%+v", p)
	}
	if err := Clear("U", "dev.x", "test"); err != nil || len(Markers("U", "dev.x")) != 0 || len(PreflightFails("U")) != 0 {
		t.Fatalf("clear: %v", err)
	}
}

func itoa(n int64) string { return strconv.FormatInt(n, 10) }
