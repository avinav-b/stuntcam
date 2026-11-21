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

// Fallback is the engine's container-tmp location for one app (engine/src/core/markers.inc, SCTmpRoot and SCLoadInit). When the
// app can't open events.log in its state dir (SIMCAM_STATE_DIR/<bundle>, normally $SIMCAM_HOME/sims/<udid>/state; on 17.5 when
// the sim can't write there), it keeps events.log, latched-<build> and latched-motion-<build> in
// <data container>/tmp/simcam-state/<bundle>/, and preflight-fail-<build> in <data container>/tmp/simcam-state/. The data
// container belongs to one app, so its simcam-state root holds that app's markers only.
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

// FallbackFor checks a data container path (from ParseAppContainer) and returns the app's fallback location in it. The path
// must be clean and be one of this simulator's app data containers (…/<udid>/data/Containers/Data/Application/<id>), so a
// clear can never reach outside it.
func FallbackFor(udid, bundle, dataContainer string) (Fallback, error) {
	if bundle == "" || bundle == "." || bundle == ".." || strings.ContainsAny(bundle, "/\x00") {
		return Fallback{}, fmt.Errorf("bad bundle id %q", bundle)
	}
	c := strings.TrimRight(dataContainer, "/")
	if !filepath.IsAbs(c) || filepath.Clean(c) != c {
		return Fallback{}, fmt.Errorf("data container %q is not a clean absolute path", dataContainer)
	}
	parent, id := filepath.Split(c)
	if want := "/" + udid + "/data/Containers/Data/Application/"; udid == "" || !strings.HasSuffix(parent, want) || id == "" {
		return Fallback{}, fmt.Errorf("data container %q is not an app data container of simulator %s", dataContainer, udid)
	}
	root := filepath.Join(c, "tmp", "simcam-state")
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

// realDir: a directory that is not a symlink (an app can write its own container; we never follow a link out of it).
func realDir(p string) bool {
	fi, err := os.Lstat(p)
	return err == nil && fi.IsDir()
}

func regular(p string) bool {
	fi, err := os.Lstat(p)
	return err == nil && fi.Mode().IsRegular()
}

// ErrSymlink: a component of the fallback path inside the container is a symlink; the CLI never follows one out of the
// container (the engine would), so that location is reported as not checked.
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

// Check: nil if the location can be read and cleared without following a symlink.
func (f Fallback) Check() error {
	if f.linked() {
		return ErrSymlink
	}
	return nil
}

// Read lists the fallback's events, markers and preflight-fail markers (the build as for Analyze).
func (f Fallback) Read(bundle, build string) FallbackState {
	st := FallbackState{Fallback: f, App: App{Bundle: bundle, Latched: map[string]int{}}}
	if f.linked() || !realDir(f.Root) {
		return st
	}
	st.PreflightFails = PreflightFailsIn(f.Root)
	if !realDir(f.Dir) {
		return st
	}
	ev := filepath.Join(f.Dir, "events.log")
	if regular(ev) {
		if a, err := LoadFile(ev, bundle, build); err == nil {
			st.Events, st.App = true, a
		}
	}
	st.Markers = MarkersIn(f.Dir)
	return st
}

// Empty: nothing the engine reads at the next launch is there.
func (s FallbackState) Empty() bool {
	return !s.Events && len(s.Markers) == 0 && len(s.PreflightFails) == 0
}

// Latches describes what in the fallback latches or stocks the given build's next launch ("" = nothing). Markers count at
// every launch (the engine checks both locations at load), so their features are returned; features counted in the fallback
// events.log only apply if the app falls back again (the engine reads that log only when it can't open its state dir), so
// they are described as such and not returned.
func (s FallbackState) Latches(build string, now time.Time) (desc string, feats []string) {
	var parts []string
	if f := s.App.LatchedFeatures(); len(f) > 0 {
		parts = append(parts, strings.Join(f, ",")+" counted in its events.log (applies if the app again can't open its state dir)")
	}
	for _, m := range s.Markers {
		if m.Build == build && m.Valid(now) {
			features := strings.ReplaceAll(m.Features, " ", ",")
			parts = append(parts, fmt.Sprintf("marker %s: %s until %s", m.Name(), features, m.Until.Format("2006-01-02 15:04")))
			feats = append(feats, strings.Fields(m.Features)...)
		}
	}
	for _, p := range s.PreflightFails {
		if p.Build == build && p.Valid(now) {
			until := p.Until.Format("2006-01-02 15:04")
			parts = append(parts, fmt.Sprintf("marker preflight-fail-%s: %s until %s (stock CoreMotion)", p.Build, p.What, until))
		}
	}
	return strings.Join(parts, "; "), feats
}

// Clear does in the fallback what Clear does in the state dir, and nothing else in the container: appends the clear line to
// the bundle's events.log if one is there (never creates one), and removes the bundle's latched-* markers and the root's
// preflight-fail-* markers. Returns what it did, sorted ("" items never).
func (f Fallback) Clear(why string) ([]string, error) {
	var done []string
	var errs []error
	if f.linked() {
		return nil, ErrSymlink
	}
	// the clear line first, as the engine's SCClearAtLoad does: an app launching meanwhile then sees the clear before the
	// markers go, instead of re-latching from the old records after they went
	if p := filepath.Join(f.Dir, "events.log"); realDir(f.Dir) && regular(p) {
		if err := AppendClear(p, why, false); err != nil {
			errs = append(errs, err)
		} else {
			done = append(done, "clear line appended to events.log")
		}
	}
	if realDir(f.Root) {
		ents, _ := os.ReadDir(f.Root)
		for _, e := range ents {
			if strings.HasPrefix(e.Name(), "preflight-fail-") && !e.IsDir() {
				if err := os.Remove(filepath.Join(f.Root, e.Name())); err != nil {
					errs = append(errs, err)
				} else {
					done = append(done, "removed "+e.Name())
				}
			}
		}
	}
	if realDir(f.Dir) {
		ents, _ := os.ReadDir(f.Dir)
		for _, e := range ents {
			if strings.HasPrefix(e.Name(), "latched-") && !e.IsDir() {
				if err := os.Remove(filepath.Join(f.Dir, e.Name())); err != nil {
					errs = append(errs, err)
				} else {
					done = append(done, "removed "+e.Name())
				}
			}
		}
	}
	sort.Strings(done)
	return done, errors.Join(errs...)
}

// AppendClear appends the clear line to an events.log as the engine's SCAppendLocked appends: under flock(LOCK_EX), and only
// to the inode the path still names (a trim renames a shorter copy over the log; a line written to the old inode would be
// lost and the kept records would re-latch the app). create: make the file if missing (the state dir; never
// in an app's container). Never follows a symlink.
func AppendClear(path, why string, create bool) error {
	line := fmt.Sprintf("%d 0 clear - - %s\n", time.Now().Unix(), strings.ReplaceAll(why, "\n", " "))
	flags := os.O_WRONLY | os.O_APPEND | syscall.O_NOFOLLOW
	if create {
		flags |= os.O_CREATE
	}
	for try := 0; try < 5; try++ {
		f, err := os.OpenFile(path, flags, 0o644)
		if err != nil {
			return err
		}
		if err := lockWait(f, LockWait); err != nil {
			f.Close()
			return fmt.Errorf("%s: %w", path, err)
		}
		fi, ferr := f.Stat()
		pi, perr := os.Lstat(path)
		if ferr == nil && perr == nil && os.SameFile(fi, pi) {
			_, err = f.WriteString(line)
			f.Close() // releases the lock
			return err
		}
		f.Close() // replaced under us (a trim): reopen the new file
	}
	return fmt.Errorf("%s kept being replaced; clear line not written", path)
}

// LockWait bounds the wait for events.log's lock: an app stopped inside its locked append or trim (a debugger breakpoint,
// a suspended app) must not hang the recovery command forever.
var LockWait = 5 * time.Second

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
		time.Sleep(50 * time.Millisecond)
	}
}
