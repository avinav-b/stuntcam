// SPDX-License-Identifier: Apache-2.0
package webcam

import (
	"bytes"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/procenv"
)

func execPath(pid int) string { return procenv.ExecPath(pid) }

// gcGrace is how old a pid file must be before GC removes it (a helper that is starting writes it before it is fully
// up).
const gcGrace = 10 * time.Second

const asideMark = ".gc."

const (
	// staleFileAge is the age from which a late --list answer, or a pid file a dead GC set aside, is removed.
	staleFileAge = time.Minute
	// helperIdleAge is how long an unused helper/<sha>/ dir must be untouched before it is removed.
	helperIdleAge = 24 * time.Hour
)

// removeIfUnchanged removes the pid file at path if it still holds data and is older than gcGrace. It renames the file
// aside first, so what it checks is what it removes: a helper that writes a new pid file meanwhile gets it back (or
// keeps the one it wrote after the rename).
func (e *Env) removeIfUnchanged(path string, data []byte) bool {
	aside := path + asideMark + strconv.Itoa(os.Getpid())
	if os.Rename(path, aside) != nil {
		return false
	}

	current, err := os.ReadFile(aside)
	info, statErr := os.Stat(aside)
	if err == nil && statErr == nil && bytes.Equal(current, data) && e.Now().Sub(info.ModTime()) >= gcGrace {
		return os.Remove(aside) == nil
	}

	os.Link(aside, path) // fails if a newer pid file is already there: that one wins
	os.Remove(aside)     // best effort: the next GC removes an aside file left behind
	return false
}

// LiveShas are the helper <sha>s that live pid files name.
func (e *Env) LiveShas() map[string]bool {
	out := map[string]bool{}
	for _, cam := range PidFiles(e.Home) {
		if pidFile, live, err := e.ReadPid(cam); err == nil && live {
			out[pidFile.Sha] = true
		}
	}
	return out
}

// GC removes what dead helpers left (their pid files, and their rings while the object at the name is still theirs:
// its header names the dead pid, so a ring a newer helper has published is never unlinked) and the helper/<sha>/ dirs
// that neither keep nor a live pid file names, once untouched for a day. A pid file newer than gcGrace is left (a
// helper may be starting), and stale `--list` answers are removed. Never a .lock file.
func (e *Env) GC(keep map[string]bool, dry bool, unlink func(string) error, producer func(string) int,
	remove func(what, path string)) {
	live := e.LiveShas()
	for _, cam := range PidFiles(e.Home) {
		pidPath := PidPath(e.Home, cam)
		data, err := os.ReadFile(pidPath)
		if err != nil {
			continue
		}
		pidFile, parseErr := ParsePid(data)
		if parseErr == nil && e.running(pidFile.Pid) {
			continue
		}
		if fi, err := os.Stat(pidPath); err != nil || e.Now().Sub(fi.ModTime()) < gcGrace {
			continue // a helper starting right now may have just written it
		}
		if parseErr == nil {
			for _, ring := range pidFile.Rings {
				name := ShmName(e.Uid, cam, ring)
				if producer(name) != pidFile.Pid {
					continue
				}
				if dry {
					e.say("would unlink the ring %s of the exited camera helper %d", name, pidFile.Pid)
				} else if unlink(name) == nil {
					e.say("unlinked the ring %s of the exited camera helper %d", name, pidFile.Pid)
				}
			}
		}
		if dry {
			e.say("would remove the pid file of an exited camera helper %s", pidPath)
			continue
		}
		if e.removeIfUnchanged(pidPath, data) {
			e.say("removed the pid file of an exited camera helper %s", pidPath)
		}
	}
	// what a `--list` answered after List gave up, and pid files a GC that died set aside
	entries, _ := os.ReadDir(Dir(e.Home)) // no dir: nothing to collect
	for _, entry := range entries {
		name := entry.Name()
		if entry.IsDir() || !(strings.HasPrefix(name, listPrefix) || strings.Contains(name, ".pid"+asideMark)) {
			continue
		}
		if fi, err := entry.Info(); err == nil && e.Now().Sub(fi.ModTime()) > staleFileAge {
			if dry {
				e.say("would remove the stale file %s", filepath.Join(Dir(e.Home), name))
			} else if os.Remove(filepath.Join(Dir(e.Home), name)) == nil {
				e.say("removed the stale file %s", filepath.Join(Dir(e.Home), name))
			}
		}
	}
	helpers, _ := os.ReadDir(HelperRoot(e.Home)) // no dir: nothing to collect
	for _, entry := range helpers {
		sha := entry.Name()
		if !entry.IsDir() || strings.HasPrefix(sha, ".") || !shaRe.MatchString(sha) || keep[sha] || live[sha] {
			continue
		}
		if fi, err := entry.Info(); err == nil && e.Now().Sub(fi.ModTime()) > helperIdleAge {
			remove("old camera helper", filepath.Join(HelperRoot(e.Home), sha))
		}
	}
}
