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

// gcGrace: GC leaves a pid file this new alone (a helper that is starting writes it before it is fully up).
const gcGrace = 10 * time.Second

const asideMark = ".gc."

// removeIfUnchanged removes the pid file at pp if it still holds b and is older than gcGrace. It renames the file aside
// first, so what it checks is what it removes: a helper that writes a new pid file meanwhile gets it back (or keeps the
// one it wrote after the rename).
func (e *Env) removeIfUnchanged(pp string, b []byte) bool {
	aside := pp + asideMark + strconv.Itoa(os.Getpid())
	if os.Rename(pp, aside) != nil {
		return false
	}
	now, err := os.ReadFile(aside)
	fi, serr := os.Stat(aside)
	if err == nil && serr == nil && bytes.Equal(now, b) && e.Now().Sub(fi.ModTime()) >= gcGrace {
		return os.Remove(aside) == nil
	}
	os.Link(aside, pp) // fails if a newer pid file is already there: that one wins
	os.Remove(aside)
	return false
}

// LiveShas are the helper <sha>s that live pid files name.
func (e *Env) LiveShas() map[string]bool {
	out := map[string]bool{}
	for _, cam := range PidFiles(e.Home) {
		if p, live, err := e.ReadPid(cam); err == nil && live {
			out[p.Sha] = true
		}
	}
	return out
}

// GC removes what dead helpers left (their pid files, and their rings while the object at the name is still theirs:
// its header names the dead pid, so a ring a newer helper has published is never unlinked) and the helper/<sha>/ dirs
// that neither keep nor a live pid file names, once untouched for a day. A pid file newer than gcGrace is left (a
// helper may be starting), and stale `--list` answers are removed. Never a .lock file.
func (e *Env) GC(keep map[string]bool, dry bool, unlink func(string) error, producer func(string) int, rm func(what, p string)) {
	live := e.LiveShas()
	for _, cam := range PidFiles(e.Home) {
		pp := PidPath(e.Home, cam)
		b, err := os.ReadFile(pp)
		if err != nil {
			continue
		}
		p, perr := ParsePid(b)
		if perr == nil && e.running(p.Pid) {
			continue
		}
		if fi, err := os.Stat(pp); err != nil || e.Now().Sub(fi.ModTime()) < gcGrace {
			continue // a helper starting right now may have just written it
		}
		if perr == nil {
			for _, v := range p.Rings {
				name := ShmName(e.Uid, cam, v)
				if producer(name) != p.Pid {
					continue
				}
				if dry {
					e.say("would unlink the ring %s of the exited camera helper %d", name, p.Pid)
				} else if unlink(name) == nil {
					e.say("unlinked the ring %s of the exited camera helper %d", name, p.Pid)
				}
			}
		}
		if dry {
			e.say("would remove the pid file of an exited camera helper %s", pp)
			continue
		}
		if e.removeIfUnchanged(pp, b) {
			e.say("removed the pid file of an exited camera helper %s", pp)
		}
	}
	// what a `--list` answered after List gave up, and pid files a GC that died set aside
	ds, _ := os.ReadDir(Dir(e.Home))
	for _, d := range ds {
		n := d.Name()
		if d.IsDir() || !(strings.HasPrefix(n, listPrefix) || strings.Contains(n, ".pid"+asideMark)) {
			continue
		}
		if fi, err := d.Info(); err == nil && e.Now().Sub(fi.ModTime()) > time.Minute {
			if dry {
				e.say("would remove the stale file %s", filepath.Join(Dir(e.Home), n))
			} else if os.Remove(filepath.Join(Dir(e.Home), n)) == nil {
				e.say("removed the stale file %s", filepath.Join(Dir(e.Home), n))
			}
		}
	}
	es, _ := os.ReadDir(HelperRoot(e.Home))
	for _, d := range es {
		n := d.Name()
		if !d.IsDir() || strings.HasPrefix(n, ".") || !shaRe.MatchString(n) || keep[n] || live[n] {
			continue
		}
		if fi, err := d.Info(); err == nil && e.Now().Sub(fi.ModTime()) > 24*time.Hour {
			rm("old camera helper", filepath.Join(HelperRoot(e.Home), n))
		}
	}
}
