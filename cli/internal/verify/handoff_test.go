// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// Tests for the device shutdown handoff, the install skip, the retried-pass cache and the post-boot settle floor.

// fakeXcrun puts an xcrun on PATH that logs its arguments to <dir>/calls, answers `list devices -j` with device U in the
// state <dir>/state holds, and on `shutdown` writes <dir>/after-shutdown (if present) into <dir>/state.
func fakeXcrun(t *testing.T, state string) string {
	dir := t.TempDir()
	script := `#!/bin/sh
d="$FAKE_XCRUN_DIR"
echo "$*" >> "$d/calls"
case "$*" in
*" shutdown U") [ -f "$d/after-shutdown" ] && cp "$d/after-shutdown" "$d/state"; exit 0;;
*" list devices -j") printf '{"devices":{"rt":[{"udid":"U","name":"n","state":"%s","isAvailable":true}]}}' "$(cat "$d/state")";;
*) exit 3;;
esac
`
	os.WriteFile(filepath.Join(dir, "xcrun"), []byte(script), 0o755)
	os.WriteFile(filepath.Join(dir, "state"), []byte(state), 0o644)
	t.Setenv("FAKE_XCRUN_DIR", dir)
	t.Setenv("PATH", dir+string(os.PathListSeparator)+os.Getenv("PATH"))
	return dir
}

func shutdownCalls(dir string) int {
	b, _ := os.ReadFile(filepath.Join(dir, "calls"))
	return strings.Count(string(b), " shutdown U")
}

func TestFinishShutdownLockOwnership(t *testing.T) {
	old := handoffGrace
	handoffGrace = 200 * time.Millisecond
	defer func() { handoffGrace = old }()
	h := holder(t) // a live process that is neither us nor our parent: a later verify

	for _, c := range []struct {
		name        string
		owner       int // the lock's pid when the child starts
		parent      int
		state       string
		release     bool // the child releases the lock
		shutdown    bool // the child shuts the device down at all
		wantErrFrag string
	}{
		{"handed to the child", os.Getpid(), 4242, "Booted", true, true, ""},
		{"still names the parent", 4242, 4242, "Booted", true, true, ""},
		{"another verify holds it", h.Process.Pid, 4242, "Booted", false, false, "is Booted"},
	} {
		t.Run(c.name, func(t *testing.T) {
			t.Setenv("SIMCAM_HOME", t.TempDir())
			os.MkdirAll(home.Root(), 0o755)
			dir := fakeXcrun(t, c.state)
			os.WriteFile(filepath.Join(dir, "after-shutdown"), []byte("Shutdown"), 0o644)
			lock := lockDir("24A434")
			holdLock(t, lock, c.owner, true)
			err := FinishShutdown("U", lock, c.parent)
			_, statErr := os.Stat(lock)
			if released := os.IsNotExist(statErr); released != c.release {
				t.Fatalf("released=%v, want %v", released, c.release)
			}
			if n := shutdownCalls(dir); (n > 0) != c.shutdown {
				t.Fatalf("%d shutdown calls, want any=%v", n, c.shutdown)
			}
			if (err == nil) != (c.wantErrFrag == "") || (err != nil && !strings.Contains(err.Error(), c.wantErrFrag)) {
				t.Fatalf("err %v, want %q", err, c.wantErrFrag)
			}
		})
	}

	// a device that keeps coming back Booted is shut down again only while the lock is ours; once another verify takes
	// the lock (here: right after our first shutdown), the child stops touching the device
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Root(), 0o755)
	dir := fakeXcrun(t, "Booted") // no after-shutdown: it stays Booted
	lock := lockDir("24A434")
	holdLock(t, lock, os.Getpid(), true)
	go func() {
		for shutdownCalls(dir) == 0 {
			time.Sleep(10 * time.Millisecond)
		}
		os.WriteFile(filepath.Join(lock, "pid"), []byte(strconv.Itoa(h.Process.Pid)+"\n"), 0o644)
	}()
	done := make(chan error, 1)
	go func() { done <- FinishShutdown("U", lock, 4242) }()
	select {
	case <-done:
	case <-time.After(10 * time.Second):
		t.Fatal("the child kept going after it lost the lock")
	}
	if n := shutdownCalls(dir); n > 3 {
		t.Fatalf("%d shutdowns after the lock changed hands", n)
	}
	if readPid(lock) != h.Process.Pid {
		t.Fatal("the child released another verify's lock")
	}
}

// the child's pid can't be written into the lock: the child is killed (so it can't shut a later verify's device
// down), the marker removed, and the caller keeps the lock for its own synchronous shutdown.
func TestHandoffKillsChildOnPidWriteFailure(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Root(), 0o755)
	d := lockDir("23D8133")
	if err := acquire(d); err != nil {
		t.Fatal(err)
	}
	var child *exec.Cmd
	oldSpawn, oldWrite := spawnShutdown, writeLockPid
	defer func() { spawnShutdown, writeLockPid = oldSpawn, oldWrite }()
	spawnShutdown = func(string, string) (int, error) {
		child = exec.Command("/bin/sleep", "30")
		child.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
		if err := child.Start(); err != nil {
			return 0, err
		}
		return child.Process.Pid, nil
	}
	writeLockPid = func(string, int) error { return fmt.Errorf("disk full") }
	run := &running{udid: "U", lock: d}
	if handoffShutdown(run) {
		t.Fatal("handed off although the pid write failed")
	}
	exited := make(chan error, 1)
	go func() { exited <- child.Wait() }()
	select {
	case err := <-exited:
		if err == nil {
			t.Fatal("child exited cleanly: not killed")
		}
	case <-time.After(3 * time.Second):
		child.Process.Kill()
		t.Fatal("child still running after a failed handoff")
	}
	if run.lock != d || run.udid != "U" || readPid(d) != os.Getpid() {
		t.Fatalf("the caller lost the lock: %+v pid %d", run, readPid(d))
	}
	if _, err := os.Stat(filepath.Join(d, shutdownMarker)); err == nil {
		t.Fatal("marker left behind")
	}
	release(d)
}

// a shutdown marker older than the child can live is a dead owner's lock, even when its pid is alive (reused).
func TestStaleShutdownMarker(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	os.MkdirAll(home.Root(), 0o755)
	d := lockDir("24A434")
	h := holder(t)
	holdLock(t, d, h.Process.Pid, false)
	os.WriteFile(filepath.Join(d, shutdownMarker), []byte(fmt.Sprintf("UDID-1 %d\n", time.Now().Add(-ShutdownLife-time.Minute).Unix())), 0o644)
	if sd, _ := shuttingDown(d); sd {
		t.Fatal("a stale marker counts as a running shutdown")
	}
	t0 := time.Now()
	if err := acquireWait(d, 10*time.Second, func(string) {}); err != nil || time.Since(t0) > 3*time.Second {
		t.Fatalf("stale lock not taken over: %v after %s", err, time.Since(t0))
	}
	release(d)
	// a fresh marker with the same live pid is waited for (TestAcquireWaitsForAShutdown), and a live verify's lock without a
	// marker is still refused
	holdLock(t, d, h.Process.Pid, false)
	if acquire(d) == nil {
		t.Fatal("a live verify's lock was taken over")
	}
	if ShutdownWait < ShutdownLife {
		t.Fatalf("ShutdownWait %s is shorter than the child's life %s", ShutdownWait, ShutdownLife)
	}
}

// a first run that never started on a verify that skipped the install or the grant reinstalls and re-grants.
func TestRedoSkippedInstall(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	var got []installPlan
	old := installGrant
	defer func() { installGrant = old }()
	installGrant = func(_ simctl.Ctl, _ string, p installPlan) error { got = append(got, p); return nil }
	rec := CanaryRecordFile("U")
	os.MkdirAll(filepath.Dir(rec), 0o755)

	for _, c := range []struct {
		name           string
		install, grant bool
		ran            bool
		want           bool
	}{
		{"skipped both, never started", false, false, false, true},
		{"skipped the grant only, never started", true, false, false, true},
		{"skipped both, but it ran", false, false, true, false},
		{"installed and granted already", true, true, false, false},
	} {
		got = nil
		os.WriteFile(rec, []byte("{}"), 0o644)
		p := installPlan{Install: c.install, Grant: c.grant}
		redone, err := redoSkippedInstall(simctl.Ctl{}, "U", &p, c.ran)
		if err != nil || redone != c.want || (len(got) == 1) != c.want {
			t.Fatalf("%s: redone=%v err=%v calls=%v", c.name, redone, err, got)
		}
		_, statErr := os.Stat(rec)
		if c.want && (!got[0].Install || !got[0].Grant || !p.Install || !p.Grant || !os.IsNotExist(statErr)) {
			t.Fatalf("%s: plan %+v, record removed=%v", c.name, got[0], os.IsNotExist(statErr))
		}
		if !c.want && os.IsNotExist(statErr) {
			t.Fatalf("%s: the install record was removed", c.name)
		}
	}
	installGrant = func(simctl.Ctl, string, installPlan) error { return fmt.Errorf("install failed: x") }
	p := installPlan{}
	if _, err := redoSkippedInstall(simctl.Ctl{}, "U", &p, false); err == nil {
		t.Fatal("install error swallowed")
	}
}

// a retried pass is kept in the cache with its retried list, used for RetriedPassTTL only; a plain pass and a fail
// are not affected; a file without the field (as an older scripts/verify.sh wrote) is read from its RESULT line.
func TestRetriedPassCache(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	now := time.Now()
	rl := "photo-jpeg: longest frame gap during the photo 304 ms (<= 100 ms, 7 frames) -> retry ok"
	put := func(key, verdict, retried, result string, age time.Duration) {
		st, _ := Classify("RESULT core=PASS")
		if verdict != "pass" {
			st = Fail
		}
		if err := store(Result{Key: key, Verdict: verdict, State: st, Retried: retried, CanaryResult: result, At: now.Add(-age).Unix()}); err != nil {
			t.Fatal(err)
		}
	}
	put("fresh", "pass", rl, "", 10*time.Minute)
	put("old", "pass", rl, "", 2*time.Hour)
	put("plain-old", "pass", "", "RESULT core=PASS ext=PASS t=14.0s", 48*time.Hour)
	put("fail-old", "fail(x)", rl, "", 48*time.Hour)
	put("sh-old", "pass", "", "RESULT core=PASS ext=PASS t=19.2s retried:["+rl+"]", 2*time.Hour)

	tests := []struct {
		name, key string
		ok        func(c cacheEntry) bool
	}{
		{"fresh retried pass is used", "fresh", func(c cacheEntry) bool {
			return c.usable && c.result.RetriedPass() && c.result.Retried == rl
		}},
		{"old retried pass expires", "old", func(c cacheEntry) bool {
			return !c.usable && strings.Contains(c.why, "canary retry")
		}},
		{"plain pass never expires", "plain-old", func(c cacheEntry) bool {
			return c.usable && !c.result.RetriedPass()
		}},
		{"retried fail never expires", "fail-old", func(c cacheEntry) bool {
			return c.usable && c.result.State == Fail
		}},
		// a file without the field, as scripts/verify.sh writes it: the retry is read from its RESULT line
		{"old retried pass from scripts/verify.sh expires", "sh-old", func(c cacheEntry) bool { return !c.usable }},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if c := cached(tt.key, now); !tt.ok(c) {
				t.Fatalf("usable=%v why=%q %+v", c.usable, c.why, c.result)
			}
		})
	}
	if _, ok := Cached("old"); ok {
		t.Fatal("Cached used an expired retried pass")
	}
}

// the post-boot settle floor.
func TestSettleFloor(t *testing.T) {
	t.Setenv("SIMCAM_VERIFY_SETTLE_S", "")
	for v, want := range map[string]time.Duration{"27.0": PostBootSettle, "27.1": PostBootSettle, "28.0": PostBootSettle, "": PostBootSettle,
		"26.3": 0, "26.3.1": 0, "17.5": 0} {
		if got := settleFloor(v); got != want {
			t.Errorf("settleFloor(%q) = %s, want %s", v, got, want)
		}
	}
	t.Setenv("SIMCAM_VERIFY_SETTLE_S", "1.5")
	if got := settleFloor("26.3"); got != 1500*time.Millisecond {
		t.Errorf("override: %s", got)
	}
	// only the remainder is waited: 1 s already passed of a 1.4 s floor
	t0 := time.Now()
	waitSettle(time.Now().Add(-time.Second), 1400*time.Millisecond)
	if e := time.Since(t0); e < 350*time.Millisecond || e > 2*time.Second {
		t.Fatalf("waited %s, want ~0.4 s", e)
	}
	t0 = time.Now()
	waitSettle(time.Now().Add(-10*time.Second), PostBootSettle) // the guard and grants took longer than the floor
	waitSettle(time.Time{}, PostBootSettle)                     // bootstatus never returned (no boot): no wait
	if e := time.Since(t0); e > 100*time.Millisecond {
		t.Fatalf("waited %s past the floor", e)
	}
}

// a pass decided by a whole relaunch is short-lived like a retried pass
func TestRelaunchedPassCache(t *testing.T) {
	t.Setenv("SIMCAM_HOME", t.TempDir())
	now := time.Now()
	r := Result{Key: "rl", Verdict: "pass", State: Pass, Relaunched: "RESULT core=INCONCLUSIVE (frames A: 3 in 1 s)", CanaryResult: "RESULT core=PASS ext=PASS", At: now.Add(-2 * time.Hour).Unix()}
	if err := store(r); err != nil {
		t.Fatal(err)
	}
	if c := cached("rl", now); c.usable || !strings.Contains(c.why, "canary retry") {
		t.Fatalf("old relaunched pass used: usable=%v why=%q", c.usable, c.why)
	}
	r.Key, r.At = "rl2", now.Add(-10*time.Minute).Unix()
	store(r)
	if c := cached("rl2", now); !c.usable || !c.result.RetriedPass() || c.result.Relaunched == "" {
		t.Fatalf("fresh relaunched pass: usable=%v %+v", c.usable, c.result)
	}
}

// a lock that can't be made at all (no $SIMCAM_HOME) fails at once instead of spinning to the limit
func TestAcquireWaitNoHome(t *testing.T) {
	dir := filepath.Join(t.TempDir(), "missing", "verify-X.lock")
	start := time.Now()
	err := acquireWait(dir, 5*time.Second, func(string) {})
	if err == nil || time.Since(start) > time.Second || !strings.HasPrefix(err.Error(), "mkdir ") {
		t.Fatalf("err=%v after %s", err, time.Since(start))
	}
}
