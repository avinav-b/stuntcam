// SPDX-License-Identifier: Apache-2.0
package state

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"syscall"
	"time"
)

// Fallback is the engine's container-tmp location for one app (SCTmpRoot and SCLoadInit in
// engine/src/core/markers.inc).
// The app's state dir is SIMCAM_STATE_DIR/<bundle>, normally $SIMCAM_HOME/sims/<udid>/state. When the app can't open
// events.log there, as on 17.5 when the sim can't write there, it uses its data container instead:
//   - events.log, latched-<build> and latched-motion-<build> go in <data container>/tmp/simcam-state/<bundle>/;
//   - preflight-fail-<build> goes in <data container>/tmp/simcam-state/.
//
// The data container belongs to one app, so its simcam-state root holds only that app's markers.
type Fallback struct {
	Root string // <data container>/tmp/simcam-state (preflight-fail-<build>)
	Dir  string // Root/<bundle> (events.log, latched-*)
}

// ParseAppContainer reads `simctl get_app_container <udid> <bundle> data`'s output: one absolute path.
func ParseAppContainer(out string) (string, error) {
	p := strings.TrimSpace(out)
	if p == "" || strings.ContainsAny(p, "\n\r") || !filepath.IsAbs(p) {
		return "", fmt.Errorf("unexpected get_app_container output %q", out)
	}
	return p, nil
}

// FallbackFor checks a data container path (from ParseAppContainer) and returns the app's fallback location in it. The
// path must be clean and be one of this simulator's app data containers
// (…/<udid>/data/Containers/Data/Application/<id>), so a clear can never reach outside it.
func FallbackFor(udid, bundle, dataContainer string) (Fallback, error) {
	if bundle == "" || bundle == "." || bundle == ".." || strings.ContainsAny(bundle, "/\x00") {
		return Fallback{}, fmt.Errorf("bad bundle id %q", bundle)
	}

	container := strings.TrimRight(dataContainer, "/")
	if !filepath.IsAbs(container) || filepath.Clean(container) != container {
		return Fallback{}, fmt.Errorf("data container %q is not a clean absolute path", dataContainer)
	}

	parent, id := filepath.Split(container)
	if want := "/" + udid + "/data/Containers/Data/Application/"; udid == "" ||
		!strings.HasSuffix(parent, want) || id == "" {
		return Fallback{}, fmt.Errorf("data container %q is not an app data container of simulator %s", dataContainer,
			udid)
	}

	root := filepath.Join(container, "tmp", "simcam-state")
	return Fallback{Root: root, Dir: filepath.Join(root, bundle)}, nil
}

// FallbackState is what the engine left in one app's fallback location.
type FallbackState struct {
	Fallback
	Events         bool // events.log exists
	App            App  // its analysis (if Events)
	Markers        []Marker
	PreflightFails []PreflightFail
}

// realDir reports whether p is a directory and not a symlink (an app can write its own container, and the CLI never
// follows a link out of it).
func realDir(p string) bool {
	fi, err := os.Lstat(p)
	return err == nil && fi.IsDir()
}

func regular(p string) bool {
	fi, err := os.Lstat(p)
	return err == nil && fi.Mode().IsRegular()
}

// ErrSymlink means a component of the fallback path inside the container is a symlink. The CLI never follows one out
// of the container (the engine would), so that location is reported as not checked.
var ErrSymlink = errors.New("a symlink in the container's tmp/simcam-state path (not followed)")

// linked reports whether <container>/tmp, Root or Dir exists as something other than a real directory (a symlink).
func (f Fallback) linked() bool {
	for _, p := range []string{filepath.Dir(f.Root), f.Root, f.Dir} {
		if fi, err := os.Lstat(p); err == nil && !fi.IsDir() {
			return true
		}
	}
	return false
}

// Check returns nil if the location can be read and cleared without following a symlink.
func (f Fallback) Check() error {
	if f.linked() {
		return ErrSymlink
	}
	return nil
}

// Read lists the fallback's events, markers and preflight-fail markers (the build as for Analyze).
func (f Fallback) Read(bundle, build string) FallbackState {
	found := FallbackState{Fallback: f, App: App{Bundle: bundle, Latched: map[string]int{}}}
	if f.linked() || !realDir(f.Root) {
		return found
	}

	found.PreflightFails = PreflightFailsIn(f.Root)
	if !realDir(f.Dir) {
		return found
	}

	eventsPath := filepath.Join(f.Dir, "events.log")
	if regular(eventsPath) {
		if app, err := LoadFile(eventsPath, bundle, build); err == nil {
			found.Events = true
			found.App = app
		}
	}

	found.Markers = MarkersIn(f.Dir)
	return found
}

// Empty reports whether nothing the engine reads at the next launch is there.
func (s FallbackState) Empty() bool {
	return !s.Events && len(s.Markers) == 0 && len(s.PreflightFails) == 0
}

// Latches describes what in the fallback latches or stocks the given build's next launch. "" means nothing does.
//   - Markers count at every launch, because the engine checks both locations at load. So their features are
//     returned.
//   - Features counted in the fallback events.log apply only if the app falls back again, because the engine reads
//     that log only when it can't open its state dir. So they are described as such, and not returned.
func (s FallbackState) Latches(build string, now time.Time) (desc string, feats []string) {
	var parts []string
	if latched := s.App.LatchedFeatures(); len(latched) > 0 {
		parts = append(parts,
			strings.Join(latched, ",")+" counted in its events.log (applies if the app again can't open its state dir)")
	}

	for _, marker := range s.Markers {
		if marker.Build == build && marker.Valid(now) {
			features := strings.ReplaceAll(marker.Features, " ", ",")
			parts = append(parts, fmt.Sprintf("marker %s: %s until %s", marker.Name(), features,
				marker.Until.Format("2006-01-02 15:04")))
			feats = append(feats, strings.Fields(marker.Features)...)
		}
	}

	for _, fail := range s.PreflightFails {
		if fail.Build == build && fail.Valid(now) {
			until := fail.Until.Format("2006-01-02 15:04")
			parts = append(parts, fmt.Sprintf("marker preflight-fail-%s: %s until %s (stock CoreMotion)", fail.Build,
				fail.What, until))
		}
	}

	return strings.Join(parts, "; "), feats
}

// Clear does in the fallback what Clear does in the state dir, and nothing else in the container: appends the clear
// line to the bundle's events.log if one is there (never creates one), and removes the bundle's latched-* markers and
// the root's preflight-fail-* markers. Returns what it did, sorted ("" items never).
func (f Fallback) Clear(reason string) ([]string, error) {
	var done []string
	var errs []error
	if f.linked() {
		return nil, ErrSymlink
	}

	// the clear line first, as the engine's SCClearAtLoad does: an app launching meanwhile then sees the clear before
	// the markers go, instead of re-latching from the old records after they went
	if eventsPath := filepath.Join(f.Dir, "events.log"); realDir(f.Dir) && regular(eventsPath) {
		if err := AppendClear(eventsPath, reason, false); err != nil {
			errs = append(errs, err)
		} else {
			done = append(done, "clear line appended to events.log")
		}
	}

	if realDir(f.Root) {
		entries, _ := os.ReadDir(f.Root) // unreadable: nothing found there
		for _, entry := range entries {
			if strings.HasPrefix(entry.Name(), "preflight-fail-") && !entry.IsDir() {
				if err := os.Remove(filepath.Join(f.Root, entry.Name())); err != nil {
					errs = append(errs, err)
				} else {
					done = append(done, "removed "+entry.Name())
				}
			}
		}
	}

	if realDir(f.Dir) {
		entries, _ := os.ReadDir(f.Dir) // unreadable: nothing found there
		for _, entry := range entries {
			if strings.HasPrefix(entry.Name(), "latched-") && !entry.IsDir() {
				if err := os.Remove(filepath.Join(f.Dir, entry.Name())); err != nil {
					errs = append(errs, err)
				} else {
					done = append(done, "removed "+entry.Name())
				}
			}
		}
	}

	sort.Strings(done)
	return done, errors.Join(errs...)
}

// AppendClear appends the clear line to an events.log, as the engine's SCAppendLocked appends. It never follows a
// symlink.
//   - It holds flock(LOCK_EX) while it writes.
//   - It writes only to the inode that the path still names. A trim renames a shorter copy over the log. A line
//     written to the old inode would be lost, and the kept records would latch the app again.
//
// create makes the file if it is missing. Use it for the state dir, never in an app's container.
func AppendClear(path, reason string, create bool) error {
	line := fmt.Sprintf("%d 0 clear - - %s\n", time.Now().Unix(), strings.ReplaceAll(reason, "\n", " "))
	flags := os.O_WRONLY | os.O_APPEND | syscall.O_NOFOLLOW
	if create {
		flags |= os.O_CREATE
	}

	for try := 0; try < appendClearTries; try++ {
		file, err := os.OpenFile(path, flags, 0o644)
		if err != nil {
			return err
		}

		if err := lockWait(file, LockWait); err != nil {
			file.Close() // nothing written: closing loses nothing
			return fmt.Errorf("%s: %w", path, err)
		}

		opened, openedErr := file.Stat()
		named, namedErr := os.Lstat(path)
		if openedErr == nil && namedErr == nil && os.SameFile(opened, named) {
			_, err = file.WriteString(line)
			file.Close() // releases the lock; the write's error is the one that counts
			return err
		}

		file.Close() // replaced under us (a trim): reopen the new file
	}

	return fmt.Errorf("%s kept being replaced; clear line not written", path)
}

// LockWait bounds the wait for events.log's lock: an app stopped inside its locked append or trim (a debugger
// breakpoint, a suspended app) must not hang the recovery command forever.
var LockWait = 5 * time.Second

const (
	// appendClearTries is how often AppendClear reopens an events.log that a trim replaced while it waited for the
	// lock.
	appendClearTries = 5
	// lockPoll is how often lockWait tries the lock again.
	lockPoll = 50 * time.Millisecond
)

func lockWait(f *os.File, wait time.Duration) error {
	deadline := time.Now().Add(wait)
	for {
		err := syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
		if err == nil {
			return nil
		}

		if err != syscall.EWOULDBLOCK || time.Now().After(deadline) {
			if err == syscall.EWOULDBLOCK {
				return fmt.Errorf("locked by a running app for over %s (stopped in a debugger?)", wait)
			}

			return err
		}

		time.Sleep(lockPoll)
	}
}
