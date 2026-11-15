// SPDX-License-Identifier: Apache-2.0
package state

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestParseAppContainer(t *testing.T) {
	if p, err := ParseAppContainer("/d/U/data/Containers/Data/Application/A1\n"); err != nil || p != "/d/U/data/Containers/Data/Application/A1" {
		t.Fatal(p, err)
	}
	for _, bad := range []string{"", "\n", "relative/path\n", "/a\n/b\n"} {
		if _, err := ParseAppContainer(bad); err == nil {
			t.Errorf("%q accepted", bad)
		}
	}
}

func TestFallbackFor(t *testing.T) {
	c := "/Users/x/Library/Developer/CoreSimulator/Devices/U/data/Containers/Data/Application/A1"
	f, err := FallbackFor("U", "dev.x", c+"/") // simctl may print a trailing slash
	if err != nil || f.Root != c+"/tmp/simcam-state" || f.Dir != c+"/tmp/simcam-state/dev.x" {
		t.Fatalf("%+v %v", f, err)
	}
	for _, tc := range []struct{ udid, bundle, c string }{
		{"V", "dev.x", c}, // another sim's container
		{"U", "dev.x", "/Users/x/Library/Developer/CoreSimulator/Devices/U/data/Containers/Bundle/Application/A1"}, // the app bundle, not data
		{"U", "dev.x", "/Users/x/Library/Developer/CoreSimulator/Devices/U/data/Containers/Data/Application/A1/../../../.."},
		{"U", "dev.x", "/Users/x/Library/Developer/CoreSimulator/Devices/U/data/Containers/Data/Application/"},
		{"U", "dev.x", "Devices/U/data/Containers/Data/Application/A1"},
		{"", "dev.x", c},
		{"U", "", c}, {"U", "..", c}, {"U", "a/b", c},
	} {
		if f, err := FallbackFor(tc.udid, tc.bundle, tc.c); err == nil {
			t.Errorf("%+v accepted: %+v", tc, f)
		}
	}
}

// fakeContainer makes <tmp>/U/data/Containers/Data/Application/A1 and returns it.
func fakeContainer(t *testing.T) string {
	c := filepath.Join(t.TempDir(), "U", "data", "Containers", "Data", "Application", "A1")
	if err := os.MkdirAll(c, 0o755); err != nil {
		t.Fatal(err)
	}
	return c
}

func write(t *testing.T, p, s string, mtime time.Time) {
	t.Helper()
	os.MkdirAll(filepath.Dir(p), 0o755)
	if err := os.WriteFile(p, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
	if !mtime.IsZero() {
		os.Chtimes(p, mtime, mtime)
	}
}

func listTree(t *testing.T, root string) []string {
	var out []string
	filepath.Walk(root, func(p string, fi os.FileInfo, err error) error {
		if err == nil && p != root {
			r, _ := filepath.Rel(root, p)
			out = append(out, r)
		}
		return nil
	})
	sort.Strings(out)
	return out
}

func TestFallbackReadAndClear(t *testing.T) {
	c := fakeContainer(t)
	f, err := FallbackFor("U", "dev.x", c)
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now()
	until := now.Add(20 * time.Hour).Truncate(time.Second)
	ts := itoa(now.Unix() - 60)
	write(t, filepath.Join(f.Dir, "events.log"), ts+" 1 activate - B1 traced=0\n"+ts+" 1 hang activate B1 phase=preflight\n"+
		ts+" 2 activate - B1 traced=0\n"+ts+" 2 hang activate B1 phase=preflight\n", time.Time{})
	write(t, filepath.Join(f.Dir, "latched-B1"), "activate\n", until)
	write(t, filepath.Join(f.Dir, "latched-motion-B1"), "motion\n", until)
	write(t, filepath.Join(f.Root, "preflight-fail-B1"), "preflight failed\n", until)
	// what must stay: the app's own files, another bundle's dir, files that only look like ours
	write(t, filepath.Join(c, "tmp", "app-cache.bin"), "x", time.Time{})
	write(t, filepath.Join(c, "Library", "Preferences", "dev.x.plist"), "x", time.Time{})
	write(t, filepath.Join(c, "Documents", "latched-B1"), "x", time.Time{})
	write(t, filepath.Join(f.Root, "dev.other", "latched-B1"), "activate\n", until)
	write(t, filepath.Join(f.Dir, "events.log.trim"), "x", time.Time{})
	os.MkdirAll(filepath.Join(f.Dir, "latched-dir"), 0o755) // a directory named like a marker: not ours to remove

	st := f.Read("dev.x", "B1")
	if !st.Events || st.App.Activations != 2 || len(st.Markers) != 2 || len(st.PreflightFails) != 1 || st.Empty() {
		t.Fatalf("%+v", st)
	}
	if got := st.App.LatchedFeatures(); len(got) != 1 || got[0] != "activate" {
		t.Fatalf("latched %v", got)
	}
	desc, feats := st.Latches("B1", now)
	for _, w := range []string{"activate", "marker latched-B1: activate until", "marker latched-motion-B1: motion", "preflight-fail-B1"} {
		if !strings.Contains(desc, w) {
			t.Errorf("Latches desc %q lacks %q", desc, w)
		}
	}
	if Effect(feats) != "no camera and stock CoreMotion" {
		t.Fatalf("feats %v", feats)
	}
	if d, _ := st.Latches("OTHER", now); strings.Contains(d, "marker") {
		t.Fatalf("another build's launch: %q", d)
	}

	before := listTree(t, c)
	done, err := f.Clear("test")
	if err != nil {
		t.Fatal(err)
	}
	if want := []string{"clear line appended to events.log", "removed latched-B1", "removed latched-motion-B1", "removed preflight-fail-B1"}; strings.Join(done, "|") != strings.Join(want, "|") {
		t.Fatalf("done %q", done)
	}
	after := listTree(t, c)
	var removed []string
	for _, p := range before {
		if !containsS(after, p) {
			removed = append(removed, p)
		}
	}
	if want := []string{"tmp/simcam-state/dev.x/latched-B1", "tmp/simcam-state/dev.x/latched-motion-B1", "tmp/simcam-state/preflight-fail-B1"}; strings.Join(removed, "|") != strings.Join(want, "|") || len(after) != len(before)-3 {
		t.Fatalf("removed %q (before %d, after %d)", removed, len(before), len(after))
	}
	st = f.Read("dev.x", "B1")
	if len(st.App.LatchedFeatures()) != 0 || len(st.Markers) != 0 || len(st.PreflightFails) != 0 {
		t.Fatalf("after clear: %+v", st)
	}
	if d, _ := st.Latches("B1", now); d != "" {
		t.Fatalf("after clear: %q", d)
	}
	b, _ := os.ReadFile(filepath.Join(f.Dir, "events.log"))
	if l := strings.Split(strings.TrimSpace(string(b)), "\n"); len(l) != 5 || !strings.Contains(l[4], " 0 clear - - test") {
		t.Fatalf("events.log %q", b)
	}
}

func TestFallbackClearNeverCreatesOrFollows(t *testing.T) {
	c := fakeContainer(t)
	f, _ := FallbackFor("U", "dev.x", c)
	// nothing there: nothing done, nothing created
	if done, err := f.Clear("t"); err != nil || len(done) != 0 {
		t.Fatal(done, err)
	}
	if _, err := os.Stat(filepath.Join(c, "tmp")); err == nil {
		t.Fatal("Clear created tmp/")
	}
	if !f.Read("dev.x", "").Empty() {
		t.Fatal("empty fallback not Empty")
	}
	// a marker but no events.log: the marker goes, no events.log appears
	write(t, filepath.Join(f.Dir, "latched-B1"), "activate\n", time.Now().Add(time.Hour))
	if done, err := f.Clear("t"); err != nil || len(done) != 1 {
		t.Fatal(done, err)
	}
	if _, err := os.Stat(filepath.Join(f.Dir, "events.log")); err == nil {
		t.Fatal("Clear created events.log")
	}
	// symlinks out of the container are never followed: a linked bundle dir, a linked events.log
	out := t.TempDir()
	write(t, filepath.Join(out, "latched-B1"), "activate\n", time.Time{})
	write(t, filepath.Join(out, "events.log"), "keep\n", time.Time{})
	if err := os.Symlink(out, filepath.Join(f.Root, "dev.link")); err != nil {
		t.Fatal(err)
	}
	fl, _ := FallbackFor("U", "dev.link", c)
	if done, err := fl.Clear("t"); len(done) != 0 || err != ErrSymlink || fl.Check() != ErrSymlink {
		t.Fatalf("followed a linked dir: %q %v", done, err)
	}
	if err := os.Symlink(filepath.Join(out, "events.log"), filepath.Join(f.Dir, "events.log")); err != nil {
		t.Fatal(err)
	}
	f.Clear("t")
	if b, _ := os.ReadFile(filepath.Join(out, "events.log")); string(b) != "keep\n" {
		t.Fatalf("appended through a symlink: %q", b)
	}
	if _, err := os.Stat(filepath.Join(out, "latched-B1")); err != nil {
		t.Fatal("removed a file outside the container")
	}
}

func containsS(xs []string, x string) bool {
	for _, y := range xs {
		if y == x {
			return true
		}
	}
	return false
}

// <container>/tmp itself a symlink: reported (Check), never read or cleared through
func TestFallbackLinkedTmp(t *testing.T) {
	c := fakeContainer(t)
	out := t.TempDir()
	write(t, filepath.Join(out, "simcam-state", "dev.x", "latched-B1"), "activate\n", time.Now().Add(time.Hour))
	write(t, filepath.Join(out, "simcam-state", "preflight-fail-B1"), "x\n", time.Now().Add(time.Hour))
	if err := os.Symlink(out, filepath.Join(c, "tmp")); err != nil {
		t.Fatal(err)
	}
	f, _ := FallbackFor("U", "dev.x", c)
	if f.Check() != ErrSymlink || !f.Read("dev.x", "B1").Empty() {
		t.Fatal("linked tmp not reported")
	}
	if done, err := f.Clear("t"); err != ErrSymlink || len(done) != 0 {
		t.Fatal(done, err)
	}
	for _, p := range []string{"simcam-state/dev.x/latched-B1", "simcam-state/preflight-fail-B1"} {
		if _, err := os.Stat(filepath.Join(out, p)); err != nil {
			t.Fatalf("%s removed through a linked tmp", p)
		}
	}
}

// features counted only in the fallback events.log apply if the app falls back again: described, not returned as an
// effect
func TestFallbackEventsOnlyLatch(t *testing.T) {
	c := fakeContainer(t)
	f, _ := FallbackFor("U", "dev.x", c)
	ts := itoa(time.Now().Unix() - 60)
	write(t, filepath.Join(f.Dir, "events.log"), ts+" 1 activate - B1 traced=0\n"+ts+" 1 hang activate B1 phase=preflight\n"+
		ts+" 2 activate - B1 traced=0\n"+ts+" 2 hang activate B1 phase=preflight\n", time.Time{})
	desc, feats := f.Read("dev.x", "B1").Latches("B1", time.Now())
	if !strings.Contains(desc, "again can't open its state dir") || len(feats) != 0 {
		t.Fatalf("%q %v", desc, feats)
	}
}

// the clear line goes to the file the path names, under the lock; never through a symlink; created only when asked
func TestAppendClear(t *testing.T) {
	d := t.TempDir()
	p := filepath.Join(d, "events.log")
	if err := AppendClear(p, "x", false); err == nil {
		t.Fatal("created without create")
	}
	if err := AppendClear(p, "a\nb", true); err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p); !strings.HasSuffix(string(b), " 0 clear - - a b\n") {
		t.Fatalf("%q", b)
	}
	l := filepath.Join(d, "link.log")
	if err := os.Symlink(p, l); err != nil {
		t.Fatal(err)
	}
	if err := AppendClear(l, "x", false); err == nil {
		t.Fatal("appended through a symlink")
	}
}

// a trim renaming a shorter copy over events.log while the CLI waits for the lock: the line lands in the new file; a lock
// held past LockWait fails instead of hanging
func TestAppendClearLockAndTrim(t *testing.T) {
	d := t.TempDir()
	p := filepath.Join(d, "events.log")
	os.WriteFile(p, []byte("old\n"), 0o644)
	held, err := os.OpenFile(p, os.O_RDWR, 0)
	if err != nil {
		t.Fatal(err)
	}
	if err := syscall.Flock(int(held.Fd()), syscall.LOCK_EX); err != nil {
		t.Fatal(err)
	}
	done := make(chan error, 1)
	go func() { done <- AppendClear(p, "t", false) }()
	time.Sleep(200 * time.Millisecond)
	os.WriteFile(p+".trim", []byte("kept\n"), 0o644)
	if err := os.Rename(p+".trim", p); err != nil { // the engine's trim, under the lock
		t.Fatal(err)
	}
	held.Close() // unlock
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if b, _ := os.ReadFile(p); !strings.HasPrefix(string(b), "kept\n") || !strings.Contains(string(b), " 0 clear - - t") {
		t.Fatalf("%q", b)
	}
	old := LockWait
	defer func() { LockWait = old }()
	LockWait = 300 * time.Millisecond
	held, _ = os.OpenFile(p, os.O_RDWR, 0)
	defer held.Close()
	syscall.Flock(int(held.Fd()), syscall.LOCK_EX)
	start := time.Now()
	if err := AppendClear(p, "t", false); err == nil || !strings.Contains(err.Error(), "locked") || time.Since(start) > 3*time.Second {
		t.Fatal(fmt.Sprint(err, time.Since(start)))
	}
}
