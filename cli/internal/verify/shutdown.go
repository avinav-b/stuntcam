// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/simctl"
)

// The private device is shut down after the verdict, not before it is returned (the shutdown took 3.2 s of every
// verify). Run hands the shutdown and the per-runtime lock to a detached child (ShutdownCommand: the CLI itself, in its own
// session, so neither the CLI exiting nor a Ctrl-C in its terminal stops it):
//   - the lock dir gets a "shutdown" marker (udid, time) and the child's pid as its owner pid; the child shuts the device
//     down, waits until simctl lists it as Shutdown, then releases the lock;
//   - the next verify for that runtime (or `up`) that finds the lock held by a live owner with that marker waits for it, up
//     to ShutdownWait, instead of returning inconclusive at once;
//   - if the child dies, its lock has a dead owner and is taken over; a device still "Shutting Down" (or booted)
//     is then waited for / shut down by the next verify before it boots it (waitDevice, arm.ArmBoot);
//   - if the child can't be started, or can't be made the lock's owner, Run kills it and shuts the device down itself;
//   - the child lives at most ShutdownLife (then exits, leaving a dead owner's lock), shuts the device down again only while
//     the lock is still its own (or its parent's), and a marker older than ShutdownLife (+ slack) is a dead owner's even if
//     its pid has been reused.

// ShutdownCommand is the hidden CLI command the detached child runs (cmd/stuntcam dispatches it to FinishShutdown).
const ShutdownCommand = "_private-shutdown"

// ShutdownLife bounds the detached child: it exits after this long whatever simctl is doing. A var for tests.
var ShutdownLife = 75 * time.Second

// staleSlack is how much older than ShutdownLife a marker must be before its lock counts as a dead owner's: the parent
// writes the marker just before it starts the child.
const staleSlack = 5 * time.Second

// ShutdownWait bounds how long a verify waits for the previous verify's device to finish shutting down: the child's whole
// life plus staleSlack, so a waiter never gives up on a child that would still finish.
var ShutdownWait = ShutdownLife + staleSlack

// handoffGrace is how long the child waits for its parent to name it the lock's owner before it decides whose lock it is.
var handoffGrace = 5 * time.Second

const shutdownMarker = "shutdown"

// spawnShutdown starts the detached child; a var so tests can stand in for it.
var spawnShutdown = func(udid, lock string) (int, error) {
	exe, err := os.Executable()
	if err != nil {
		return 0, err
	}
	_ = os.MkdirAll(home.Logs(), 0o755)
	lf, err := os.OpenFile(filepath.Join(home.Logs(), "private-shutdown.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return 0, err
	}
	defer lf.Close()
	cmd := exec.Command(exe, ShutdownCommand, "--udid", udid, "--lock", lock, "--parent", strconv.Itoa(os.Getpid()))
	cmd.Stdout, cmd.Stderr = lf, lf
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		return 0, err
	}
	pid := cmd.Process.Pid
	_ = cmd.Process.Release()
	return pid, nil
}

// handoffShutdown passes the device's shutdown and the lock to a detached child. false: nothing handed off (keep, no
// device, no lock, or the child didn't start); the caller's cleanup then does it.
func handoffShutdown(run *running) bool {
	run.mu.Lock()
	defer run.mu.Unlock()
	if run.udid == "" || run.keep || run.lock == "" || readPid(run.lock) != os.Getpid() {
		return false
	}
	marker := filepath.Join(run.lock, shutdownMarker)
	if err := os.WriteFile(marker, []byte(fmt.Sprintf("%s %d\n", run.udid, time.Now().Unix())), 0o644); err != nil {
		return false
	}
	pid, err := spawnShutdown(run.udid, run.lock)
	if err != nil {
		_ = os.Remove(marker)
		return false
	}
	if err := writeLockPid(run.lock, pid); err != nil {
		// the child is running but doesn't own the lock: kill it (its own session, so its simctl too), so it can't shut a
		// later verify's boot of this device down; keep the lock, and let our cleanup's synchronous shutdown and release run
		killChild(pid)
		_ = os.Remove(marker)
		mark("shutdown-handoff failed: %v; killed %d", err, pid)
		return false
	}
	mark("shutdown-handoff pid=%d", pid)
	run.udid, run.lock = "", ""
	return true
}

// writeLockPid names a new owner in a lock dir; a var so tests can make it fail.
var writeLockPid = func(lock string, pid int) error {
	return home.WriteAtomic(filepath.Join(lock, "pid"), []byte(strconv.Itoa(pid)+"\n"))
}

// killChild kills the detached child's process group (it is a session leader, so pgid = pid).
var killChild = func(pid int) {
	_ = syscall.Kill(-pid, syscall.SIGKILL)
	_ = syscall.Kill(pid, syscall.SIGKILL)
}

// FinishShutdown is the detached child: shut the device down, wait until it is listed as Shutdown, and release the lock if
// it was handed to us (or still names the parent that handed it over). It gives up after ShutdownLife (exit 1, the lock
// left to be taken over), and it shuts the device down again only while the lock is still ours or our parent's: once
// another verify holds it, the device is that verify's.
func FinishShutdown(udid, lock string, parent int) error {
	t0 := time.Now()
	logf := func(f string, a ...any) {
		now := time.Now().Format("2006-01-02 15:04:05")
		fmt.Printf("%s %s pid=%d +%.1fs %s\n", now, udid, os.Getpid(), time.Since(t0).Seconds(), fmt.Sprintf(f, a...))
	}
	life := time.AfterFunc(ShutdownLife, func() {
		logf("gave up after %s; the lock is left to the next verify", ShutdownLife)
		os.Exit(1)
	})
	defer life.Stop()
	owned := func() bool { p := readPid(lock); return p != 0 && (p == os.Getpid() || p == parent) }
	ctl := simctl.Ctl{Set: home.Devices(), Timeout: ShutdownLife}
	var err error
	if owned() {
		err = ctl.Shutdown(udid)
		logf("shutdown: %v", err)
	} else {
		logf("lock %s not ours or our parent's (pid %d): device left alone", lock, readPid(lock))
	}
	// confirm it is listed as Shutdown (a shutdown that returned early gets another one, while the lock is ours)
	state := deviceState(ctl, udid)
	for state != "Shutdown" && state != "missing" && owned() {
		if state == "Booted" {
			_ = ctl.Shutdown(udid)
		}
		time.Sleep(500 * time.Millisecond)
		state = deviceState(ctl, udid)
	}
	logf("state %s", state)
	// the parent writes our pid right after starting us; give it a moment before deciding whose lock this is
	for end := time.Now().Add(handoffGrace); readPid(lock) != os.Getpid() && time.Now().Before(end); {
		time.Sleep(50 * time.Millisecond)
	}
	if p := readPid(lock); p == os.Getpid() || (p == parent && parent != 0) {
		os.RemoveAll(lock)
		logf("released %s", lock)
	} else {
		logf("lock %s not ours (pid %d): left alone", lock, p)
	}
	if state != "Shutdown" {
		return fmt.Errorf("device %s is %s after the shutdown", udid, state)
	}
	return err
}

func deviceState(ctl simctl.Ctl, udid string) string {
	ds, err := ctl.Devices()
	if err != nil {
		return "unknown (" + err.Error() + ")"
	}
	for _, d := range ds {
		if d.UDID == udid {
			return d.State
		}
	}
	return "missing"
}

// readMarker returns a lock's shutdown marker (udid, when it was written); ok false if there is none.
func readMarker(dir string) (udid string, at time.Time, ok bool) {
	b, err := os.ReadFile(filepath.Join(dir, shutdownMarker))
	if err != nil {
		return "", time.Time{}, false
	}
	f := strings.Fields(string(b) + " ")
	if len(f) > 1 {
		if s, err := strconv.ParseInt(f[1], 10, 64); err == nil {
			at = time.Unix(s, 0)
		}
	}
	return f[0], at, true
}

// markerStale: the lock has a shutdown marker older than the child can live, so its owner pid (if alive) is not that
// child any more (a reused pid would otherwise hold the lock for good).
func markerStale(dir string) bool {
	_, at, ok := readMarker(dir)
	return ok && !at.IsZero() && time.Since(at) > ShutdownLife+staleSlack
}

// shuttingDown says whether a held lock's owner is a detached shutdown (FinishShutdown) that is still running.
func shuttingDown(dir string) (bool, string) {
	udid, _, ok := readMarker(dir)
	if !ok || markerStale(dir) {
		return false, udid
	}
	p := readPid(dir)
	return p != 0 && pidAlive(p), udid
}

// lockPoll is how often acquireWait tries the lock again.
const lockPoll = 200 * time.Millisecond

// acquireWait is acquire, except that a lock held by the previous verify's device shutdown is waited for, up to max.
func acquireWait(dir string, max time.Duration, say func(string)) error {
	start := time.Now()
	said := false
	for {
		err := acquire(dir)
		if err == nil {
			if said {
				mark("lock-wait %.1f", time.Since(start).Seconds())
			}
			return nil
		}
		var noDir lockDirError
		if errors.As(err, &noDir) { // the lock can't be made at all: no waiting
			return err
		}
		sd, udid := shuttingDown(dir)
		if !sd {
			if _, statErr := os.Stat(dir); os.IsNotExist(statErr) && time.Since(start) < max {
				time.Sleep(lockPoll) // released between acquire's look and ours (the child finished): try again
				continue
			}
			return err
		}
		if time.Since(start) >= max {
			return fmt.Errorf("the previous verify's device %s is still shutting down after %s; lock %s",
				udid, max.Round(time.Second), dir)
		}
		if !said {
			said = true
			say(fmt.Sprintf("waiting for the previous verify's device %s to finish shutting down", udid))
		}
		time.Sleep(lockPoll)
	}
}

// waitDevice waits (at most max) while a device is in a transitional state ("Shutting Down", "Booting", "Creating"), as
// one left by a shutdown that didn't finish. It returns the last state seen.
func waitDevice(ctl simctl.Ctl, udid, state string, max time.Duration, say func(string)) string {
	transitional := func(s string) bool { return s == "Shutting Down" || s == "Booting" || s == "Creating" }
	if !transitional(state) {
		return state
	}
	say(fmt.Sprintf("the private device %s is %s; waiting for it", udid, strings.ToLower(state)))
	t0 := time.Now()
	for end := time.Now().Add(max); transitional(state) && time.Now().Before(end); {
		time.Sleep(500 * time.Millisecond)
		state = deviceState(ctl, udid)
	}
	mark("device-wait %.1f state=%s", time.Since(t0).Seconds(), state)
	return state
}
