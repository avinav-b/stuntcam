// SPDX-License-Identifier: Apache-2.0
package main

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
	"time"
)

func TestAlwaysKeptBuild(t *testing.T) { // gc keeps the runner hook's dir like a stub's
	d := t.TempDir()
	for _, n := range []string{"stub", "hook", "engine"} {
		os.MkdirAll(filepath.Join(d, n), 0o755)
	}
	os.WriteFile(filepath.Join(d, "stub", "libsimcamstub.dylib"), nil, 0o644)
	os.WriteFile(filepath.Join(d, "hook", "libsimcamrunnerhook.dylib"), nil, 0o644)
	os.WriteFile(filepath.Join(d, "engine", "libsimcam.dylib"), nil, 0o644)
	if !alwaysKeptBuild(filepath.Join(d, "stub")) || !alwaysKeptBuild(filepath.Join(d, "hook")) || alwaysKeptBuild(filepath.Join(d, "engine")) {
		t.Fatal()
	}
}

// gc keeps the current unpacked canary and the newest 2 others, and removes .stage-*/.old-* dirs
// older than an hour (only those), never the lock file.
func TestGCCanaries(t *testing.T) {
	d := t.TempDir()
	now := time.Now()
	mk := func(name string, age time.Duration) {
		p := filepath.Join(d, name)
		os.MkdirAll(filepath.Join(p, "m10canary.app"), 0o755)
		os.Chtimes(p, now.Add(-age), now.Add(-age))
	}
	mk("aaaaaaaaaaaa", 10*time.Hour) // current, oldest
	mk("bbbbbbbbbbbb", 1*time.Hour)
	mk("cccccccccccc", 2*time.Hour)
	mk("dddddddddddd", 30*time.Hour) // old enough (untouched > 24 h)
	mk("eeeeeeeeeeee", 40*time.Hour)
	mk("ffffffffffff", 5*time.Hour) // beyond the newest 2 but touched today: kept
	mk(".stage-1", 2*time.Hour)
	mk(".old-1", 2*time.Hour)
	mk(".stage-2", time.Minute) // an unpack in progress
	os.WriteFile(filepath.Join(d, ".lock"), nil, 0o644)
	var gone []string
	rm := func(what, p string) { gone = append(gone, filepath.Base(p)); os.RemoveAll(p) }
	gcCanaries(d, "aaaaaaaaaaaa", 2, now, false, rm)
	sort.Strings(gone)
	if strings.Join(gone, " ") != ".old-1 .stage-1 dddddddddddd eeeeeeeeeeee" {
		t.Fatalf("removed %v", gone)
	}
	left, _ := os.ReadDir(d)
	var names []string
	for _, e := range left {
		names = append(names, e.Name())
	}
	if strings.Join(names, " ") != ".lock .stage-2 aaaaaaaaaaaa bbbbbbbbbbbb cccccccccccc ffffffffffff" {
		t.Fatalf("left %v", names)
	}
	// no canary dir: nothing to do, nothing created
	gcCanaries(filepath.Join(d, "none"), "", 2, now, false, rm)
	if _, err := os.Stat(filepath.Join(d, "none")); err == nil {
		t.Fatal("created the canary dir")
	}
}
