// SPDX-License-Identifier: Apache-2.0
package verify

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// The per-runtime verify lock, shared with scripts/verify.sh: one verify per runtime build at a time. The lock is a
// directory with the owner's pid file, taken after the cache read and released only by its owner; a dead owner's lock
// is taken over.

func lockDir(runtimeBuild string) string {
	return filepath.Join(home.Root(), "verify-"+runtimeBuild+".lock")
}

func pidAlive(pid int) bool { return pid > 0 && syscall.Kill(pid, 0) == nil }

func readPid(dir string) int {
	b, _ := os.ReadFile(filepath.Join(dir, "pid")) // missing or garbled: pid 0, no owner
	p, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	return p
}

// pidlessLockAge is how long a lock may go without a pid file before it counts as a dead owner's: its owner died
// between the mkdir and writing the pid.
const pidlessLockAge = 5 * time.Minute

// errLockTakenOver means another verify made the lock between this one's look at it and its mkdir.
var errLockTakenOver = errors.New("lock taken over by another verify")

// lockDirError is an acquire error from a lock directory that can't be made at all (no $SIMCAM_HOME, EACCES, ENOSPC):
// waiting won't help. Its text is the mkdir error's.
type lockDirError struct{ err error }

func (e lockDirError) Error() string { return e.err.Error() }
func (e lockDirError) Unwrap() error { return e.err }

// acquire takes the lock, or says why it can't (who holds it).
func acquire(dir string) error {
	if err := os.Mkdir(dir, 0o755); err != nil {
		if !errors.Is(err, os.ErrExist) {
			return lockDirError{err}
		}
		if err := takeOver(dir); err != nil {
			return err
		}
	}
	return writeOwnPid(dir)
}

// lockPidWait is how long takeOver gives a new lock's owner to write its pid.
const lockPidWait = time.Second

// takeOver deals with a lock directory that exists already: if it was released meanwhile, or its owner is dead, it
// makes the directory anew (nil); else it says who holds the lock.
func takeOver(dir string) error {
	pid := readPid(dir)
	if pid == 0 { // its owner may not have written the pid yet
		time.Sleep(lockPidWait)
		pid = readPid(dir)
	}

	if _, err := os.Stat(dir); pid == 0 && errors.Is(err, os.ErrNotExist) { // released meanwhile: try again
		return mkdirAgain(dir)
	}

	if !lockAbandoned(dir, pid) && (pid == 0 || pidAlive(pid)) {
		owner := ""
		if pid != 0 {
			owner = fmt.Sprintf(", pid %d", pid)
		}

		return fmt.Errorf("another verify is using this runtime's device%s; lock %s", owner, dir)
	}

	os.RemoveAll(dir) // a dead owner's
	return mkdirAgain(dir)
}

// lockAbandoned says whether a lock's owner can't be holding it any more although its pid (if any) may be alive: no pid
// for over pidlessLockAge, or a shutdown child's lock older than the child can live (its pid was reused).
func lockAbandoned(dir string, pid int) bool {
	if pid == 0 {
		fi, err := os.Stat(dir)
		return err == nil && time.Since(fi.ModTime()) > pidlessLockAge
	}
	return markerStale(dir)
}

func mkdirAgain(dir string) error {
	if os.Mkdir(dir, 0o755) != nil {
		return errLockTakenOver
	}
	return nil
}

func writeOwnPid(dir string) error {
	if err := os.WriteFile(filepath.Join(dir, "pid"), []byte(strconv.Itoa(os.Getpid())+"\n"), 0o644); err != nil {
		os.RemoveAll(dir) // the write's error is the one returned
		return err
	}
	return nil
}

func release(dir string) {
	if readPid(dir) == os.Getpid() {
		os.RemoveAll(dir) // best effort: a lock left behind is taken over once its owner is dead
	}
}
