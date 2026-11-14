// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// a live process standing in for the detached shutdown child
func holder(t *testing.T) *exec.Cmd {
	c := exec.Command("/bin/sleep", "30")
	if err := c.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.Process.Kill(); c.Wait() })
	return c
}

func holdLock(t *testing.T, dir string, pid int, marker bool) {
	os.Mkdir(dir, 0o755)
	os.WriteFile(filepath.Join(dir, "pid"), []byte(strconv.Itoa(pid)+"\n"), 0o644)
	if marker {
		os.WriteFile(filepath.Join(dir, shutdownMarker), []byte("UDID-1 "+strconv.FormatInt(time.Now().Unix(), 10)+"\n"), 0o644)
	}
}

func TestAcquireWaitsForAShutdown(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Root(), 0o755)
	d := lockDir("24A434")
	h := holder(t)
	holdLock(t, d, h.Process.Pid, true)
	go func() { time.Sleep(600 * time.Millisecond); os.RemoveAll(d) }() // the child releases it
	var said []string
	t0 := time.Now()
	ok, why := acquireWait(d, 10*time.Second, func(s string) { said = append(said, s) })
	if !ok || time.Since(t0) < 500*time.Millisecond {
		t.Fatalf("ok=%v why=%q after %s", ok, why, time.Since(t0))
	}
	if len(said) != 1 || !strings.Contains(said[0], "UDID-1") {
		t.Fatalf("progress: %q", said)
	}
	if readPid(d) != os.Getpid() {
		t.Fatal("lock not ours")
	}
	release(d)

	// a live verify (no marker) is not waited for: inconclusive at once, as before
	holdLock(t, d, h.Process.Pid, false)
	t0 = time.Now()
	if ok, why := acquireWait(d, 10*time.Second, func(string) {}); ok || time.Since(t0) > 2*time.Second || !strings.Contains(why, "another verify") {
		t.Fatalf("live verify: ok=%v why=%q after %s", ok, why, time.Since(t0))
	}
	os.RemoveAll(d)

	// a shutdown that takes too long: bounded
	holdLock(t, d, h.Process.Pid, true)
	if ok, why := acquireWait(d, 500*time.Millisecond, func(string) {}); ok || !strings.Contains(why, "still shutting down") {
		t.Fatalf("bounded: ok=%v why=%q", ok, why)
	}
	// its child died: the dead owner's lock is taken over at once
	h.Process.Kill()
	h.Wait()
	if ok, why := acquireWait(d, 10*time.Second, func(string) {}); !ok {
		t.Fatal("dead shutdown child's lock not taken over: " + why)
	}
	if _, err := os.Stat(filepath.Join(d, shutdownMarker)); err == nil {
		t.Fatal("taken-over lock kept the old marker")
	}
}

func TestHandoffShutdown(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Root(), 0o755)
	d := lockDir("23D8133")
	if ok, why := acquire(d); !ok {
		t.Fatal(why)
	}
	h := holder(t)
	var gotUDID, gotLock string
	old := spawnShutdown
	defer func() { spawnShutdown = old }()
	spawnShutdown = func(u, l string) (int, error) { gotUDID, gotLock = u, l; return h.Process.Pid, nil }

	run := &running{udid: "UDID-2", lock: d}
	if !handoffShutdown(run) {
		t.Fatal("not handed off")
	}
	if gotUDID != "UDID-2" || gotLock != d || run.udid != "" || run.lock != "" {
		t.Fatalf("spawned %q %q; run %+v", gotUDID, gotLock, run)
	}
	if readPid(d) != h.Process.Pid {
		t.Fatal("lock owner is not the child")
	}
	if sd, u := shuttingDown(d); !sd || u != "UDID-2" {
		t.Fatalf("marker: %v %q", sd, u)
	}
	run.cleanup() // the CLI's own cleanup must leave the child's lock alone
	if _, err := os.Stat(d); err != nil {
		t.Fatal("cleanup released the child's lock")
	}
	os.RemoveAll(d)

	// --keep, or a child that can't start: nothing handed off, the caller's cleanup does it
	acquire(d)
	if handoffShutdown(&running{udid: "U", lock: d, keep: true}) {
		t.Fatal("handed off with keep")
	}
	spawnShutdown = func(string, string) (int, error) { return 0, os.ErrNotExist }
	run = &running{udid: "U", lock: d}
	if handoffShutdown(run) || run.lock != d || readPid(d) != os.Getpid() {
		t.Fatal("failed spawn changed the lock")
	}
	if _, err := os.Stat(filepath.Join(d, shutdownMarker)); err == nil {
		t.Fatal("failed spawn left the marker")
	}
	release(d)
}
