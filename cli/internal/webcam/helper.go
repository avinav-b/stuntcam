// SPDX-License-Identifier: Apache-2.0
package webcam

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strings"
	"syscall"
	"time"

	"github.com/avinav-b/stuntcam/cli/internal/home"
)

// Env is everything the helper's lifecycle touches outside this package; tests replace the functions.
type Env struct {
	Home     string                                // $SIMCAM_HOME, absolute
	Rings    []int                                 // ring versions the engine accepts
	Uid      int                                   // for the shm name
	Open     func(app string, args []string) error // `open -n -g <app> --args <args>`: the only way the helper is started
	Verify   func(app string) error                // codesign --verify --strict
	Alive    func(pid int) bool                    // kill(pid, 0)
	Term     func(pid int) error                   // SIGTERM
	IsHelper func(pid int) bool                    // pid runs a helper bundle from $SIMCAM_HOME/helper/
	ShmOwner func(name string) (int, error)
	Now      func() time.Time
	Sleep    func(time.Duration)
	Out      io.Writer

	StartWait, PermWait, StopWait, Poll time.Duration
}

// Real is the Env for this Mac.
func Real(home string, out io.Writer) *Env {
	return &Env{
		Home:  home,
		Rings: EngineRings,
		Uid:   os.Getuid(),
		Open: func(app string, args []string) error {
			b, err := exec.Command("/usr/bin/open",
				append([]string{"-n", "-g", app, "--args"}, args...)...).CombinedOutput()
			if err != nil {
				return fmt.Errorf("open %s: %w %s", app, err, strings.TrimSpace(string(b)))
			}

			return nil
		},
		Verify: func(app string) error {
			if b, err := exec.Command("/usr/bin/codesign", "--verify", "--strict", app).CombinedOutput(); err != nil {
				return fmt.Errorf("codesign --verify --strict %s: %s", app, strings.TrimSpace(string(b)))
			}

			return nil
		},
		Alive: func(pid int) bool {
			err := syscall.Kill(pid, 0)
			return err == nil || err == syscall.EPERM
		},
		Term:      func(pid int) error { return syscall.Kill(pid, syscall.SIGTERM) },
		IsHelper:  func(pid int) bool { return IsHelperPath(home, execPath(pid)) },
		ShmOwner:  ShmOwner,
		Now:       time.Now,
		Sleep:     time.Sleep,
		Out:       out,
		StartWait: 5 * time.Second,
		PermWait:  60 * time.Second,
		StopWait:  5 * time.Second,
		Poll:      100 * time.Millisecond,
	}
}

// IsHelperPath reports whether exe is a helper bundle's executable under home/helper/<sha>/.
func IsHelperPath(home, exe string) bool {
	if exe == "" {
		return false
	}

	// the kernel reports the resolved path; resolve home the same way (both, or neither if exe doesn't resolve)
	if e, err := filepath.EvalSymlinks(exe); err == nil {
		if h, err := filepath.EvalSymlinks(home); err == nil {
			exe = e
			home = h
		}
	}

	rel, err := filepath.Rel(HelperRoot(home), exe)
	if err != nil {
		return false
	}

	parts := strings.Split(filepath.ToSlash(rel), "/")
	return len(parts) == 5 && shaRe.MatchString(parts[0]) && parts[1] == AppName &&
		strings.Join(parts[2:], "/") == ExeRel
}

func (e *Env) say(format string, a ...any) {
	if e.Out != nil {
		fmt.Fprintf(e.Out, format+"\n", a...)
	}
}

// running reports whether pid is alive and runs a helper bundle from $SIMCAM_HOME/helper/. kill(pid, 0) alone isn't
// enough: after a helper crashes its pid file stays, and the pid can be reused by an unrelated process.
func (e *Env) running(pid int) bool { return pid > 0 && e.Alive(pid) && e.IsHelper(pid) }

// ReadPid reads camera hash cam's pid file and whether its pid is a running helper (see running).
func (e *Env) ReadPid(cam string) (PidFile, bool, error) {
	b, err := os.ReadFile(PidPath(e.Home, cam))
	if err != nil {
		return PidFile{}, false, err
	}
	p, err := ParsePid(b)
	if err != nil {
		return p, false, err
	}
	return p, e.running(p.Pid), nil
}

// listPrefix names the files `--list` writes into Dir.
const listPrefix = "cameras-"

// List asks the helper at app for the Mac's cameras (`--list`, through open like every launch).
func (e *Env) List(app string) ([]Camera, error) {
	if err := e.checkApp(app); err != nil {
		return nil, err
	}

	if err := os.MkdirAll(Dir(e.Home), 0o700); err != nil {
		return nil, err
	}

	out := filepath.Join(Dir(e.Home), fmt.Sprintf("%s%d-%d", listPrefix, os.Getpid(), e.Now().UnixNano()))
	defer os.Remove(out) // a helper that answers after the timeout leaves the file: GC removes it

	if err := e.Open(app, []string{"--home", e.Home, "--list", out}); err != nil {
		return nil, err
	}

	for start := e.Now(); e.Now().Sub(start) < e.StartWait; e.Sleep(e.Poll) {
		if b, err := os.ReadFile(out); err == nil {
			return ParseList(b)
		}
	}

	return nil, fmt.Errorf("the camera helper didn't list the cameras within %s", e.StartWait)
}

func (e *Env) checkApp(app string) error {
	if app == "" {
		return errors.New("this build has no camera helper (in a checkout: helper/build.sh)")
	}

	if !filepath.IsAbs(app) {
		return fmt.Errorf("camera helper path %s is not absolute", app)
	}

	if fi, err := os.Stat(filepath.Join(app, ExeRel)); err != nil || fi.Mode()&0o111 == 0 {
		return fmt.Errorf("no camera helper executable in %s", app)
	}

	// a helper running from anywhere else is never recognised as live (IsHelper), so Ensure would time out and gc would
	// remove its ring: refuse it up front
	if !IsHelperPath(e.Home, filepath.Join(app, ExeRel)) {
		return fmt.Errorf("camera helper %s is not under %s/<sha>/: rebuild it with this SIMCAM_HOME (helper/build.sh)",
			app, HelperRoot(e.Home))
	}

	return e.Verify(app)
}

// ErrDenied means the user denied camera access to the helper (or it is restricted).
var ErrDenied = errors.New("camera access for Stuntcam Camera is denied: allow it in " + SettingsPath + ", then run " +
	"this command again")

// ErrNoAnswer means the camera prompt wasn't answered in time.
var ErrNoAnswer = errors.New("no answer to the camera prompt: answer it (Stuntcam Camera), then run this command again")

// Ensure makes sure a helper that can serve this engine runs for camera: it reuses a running one, replaces one that
// shares no ring version with the engine, or starts the helper at app with `open -n -g`. Then it waits for the camera
// permission (synthetic: none).
func (e *Env) Ensure(app string, camera Camera) (PidFile, error) {
	cam := CamHash(e.Home, camera.ID)
	previousPid := 0
	if existing, live, err := e.ReadPid(cam); err == nil && live && existing.Auth != "denied" {
		if SharedRing(existing.Rings, e.Rings) > 0 {
			e.say("camera helper %s already running for %s (pid %d)", existing.Sha, camera.label(), existing.Pid)
			return e.waitAuth(cam, existing, camera)
		}

		e.say("the running camera helper %s (pid %d) serves ring %s, this stuntcam's engine reads %s: replacing it",
			existing.Sha, existing.Pid, ringsText(existing.Rings), ringsText(e.Rings))
		if err := e.Stop(cam); err != nil {
			return existing, err
		}

		previousPid = existing.Pid
	} else if err == nil {
		previousPid = existing.Pid
		// a helper that was just denied exits 3 s later still holding its lock: a new one started meanwhile would quit
		for start := e.Now(); live && e.Now().Sub(start) < e.StopWait; e.Sleep(e.Poll) {
			live = e.running(existing.Pid)
		}
	}

	if err := e.checkApp(app); err != nil {
		return PidFile{}, err
	}

	args := []string{"--home", e.Home}
	if camera.ID == SyntheticID {
		args = append(args, "--synthetic")
	} else {
		args = append(args, "--camera", camera.ID)
	}

	openedAt := e.Now()
	if err := e.Open(app, args); err != nil {
		return PidFile{}, err
	}

	started, err := e.waitStart(cam, openedAt, previousPid)
	if err != nil {
		return started, err
	}

	if SharedRing(started.Rings, e.Rings) == 0 {
		return started, fmt.Errorf("webcam: update stuntcam (the camera helper %s serves ring %s, this engine "+
			"accepts %s)", started.Sha, ringsText(started.Rings), ringsText(e.Rings))
	}

	e.say("camera helper %s started for %s (pid %d)", started.Sha, camera.label(), started.Pid)
	return e.waitAuth(cam, started, camera)
}

func (c Camera) label() string {
	if c.Name != "" && c.Name != c.ID {
		return fmt.Sprintf("'%s' (%s)", c.Name, c.ID)
	}
	return c.ID
}

// waitStart waits for the pid file of a helper started at openedAt (or of whichever helper won the lock meanwhile).
func (e *Env) waitStart(cam string, openedAt time.Time, previousPid int) (PidFile, error) {
	for {
		pidFile, live, err := e.ReadPid(cam)
		fresh := err == nil && (pidFile.Started >= openedAt.Unix()-1 || pidFile.Pid != previousPid)
		if fresh && (live || pidFile.Auth == "denied") {
			return pidFile, nil
		}

		if e.Now().Sub(openedAt) >= e.StartWait {
			return pidFile, e.failure(cam, fmt.Sprintf("the camera helper didn't start within %s", e.StartWait))
		}

		e.Sleep(e.Poll)
	}
}

// waitAuth polls the pid file's auth= until the helper is authorized, denied, or PermWait passes.
func (e *Env) waitAuth(cam string, pidFile PidFile, camera Camera) (PidFile, error) {
	if camera.ID == SyntheticID {
		return pidFile, nil
	}

	start, said := e.Now(), false
	for {
		switch pidFile.Auth {
		case "authorized":
			if said {
				e.say("camera access allowed")
			}

			return pidFile, nil
		case "denied":
			return pidFile, ErrDenied
		case "prompting":
			if !said {
				e.say("allow camera access for Stuntcam Camera in the dialog (waiting up to %s)", e.PermWait)
				said = true
			}
		}

		if e.Now().Sub(start) >= e.PermWait {
			return pidFile, ErrNoAnswer
		}

		e.Sleep(e.Poll)
		next, live, err := e.ReadPid(cam)
		if err != nil || (!live && next.Auth != "denied") {
			return pidFile, e.failure(cam, "the camera helper exited")
		}

		pidFile = next
	}
}

// failureLogLines is how many of the helper log's last lines failure searches for a known reason.
const failureLogLines = 20

// failure explains why a helper isn't running: another user's shm object at the ring's name, or the helper log's last
// line.
func (e *Env) failure(cam, what string) error {
	for _, ring := range e.Rings {
		name := ShmName(e.Uid, cam, ring)
		uid, err := e.ShmOwner(name)
		switch {
		case err == nil && uid != e.Uid:
			return fmt.Errorf("%s: webcam: shm name %s owned by uid %d", what, name, uid)
		case errors.Is(err, syscall.EACCES):
			return fmt.Errorf("%s: webcam: shm name %s owned by another user", what, name)
		}
	}

	logPath := LogPath(e.Home, cam)
	if data, err := os.ReadFile(logPath); err == nil {
		lines := strings.Split(strings.TrimSpace(string(data)), "\n")
		for i := len(lines) - 1; i >= 0 && i >= len(lines)-failureLogLines; i-- {
			if strings.Contains(lines[i], "owned by uid") || strings.Contains(lines[i], "no camera") ||
				strings.Contains(lines[i], "another helper") {
				_, msg, _ := strings.Cut(lines[i], "] ")
				return fmt.Errorf("%s: %s (log: %s)", what, msg, logPath)
			}
		}

		return fmt.Errorf("%s (log: %s)", what, logPath)
	}

	return errors.New(what)
}

// Stop ends the helper named by camera hash cam's pid file: SIGTERM (it clears and unlinks its ring and removes the pid
// file), then waits StopWait for it to exit. A pid that isn't a helper from $SIMCAM_HOME/helper/ is never signalled.
func (e *Env) Stop(cam string) error {
	pidFile, live, err := e.ReadPid(cam)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}

	if err != nil {
		return fmt.Errorf("%s: %w", PidPath(e.Home, cam), err)
	}

	if !live {
		return nil
	}

	if !e.IsHelper(pidFile.Pid) { // re-checked right before the signal: the pid may have been reused since ReadPid
		return fmt.Errorf("pid %d in %s is not a camera helper from %s: not stopping it", pidFile.Pid,
			PidPath(e.Home, cam), HelperRoot(e.Home))
	}

	if err := e.Term(pidFile.Pid); err != nil {
		return fmt.Errorf("stopping the camera helper (pid %d): %w", pidFile.Pid, err)
	}

	for start := e.Now(); e.running(pidFile.Pid); e.Sleep(e.Poll) {
		if e.Now().Sub(start) >= e.StopWait {
			return fmt.Errorf("the camera helper (pid %d) is still running %s after SIGTERM", pidFile.Pid, e.StopWait)
		}
	}

	return nil
}

// WriteDefault records id as the camera a bare `webcam` source means (docs/webcam-ring.md §8a).
func (e *Env) WriteDefault(id string) error { return writeAtomic(DefaultPath(e.Home), []byte(id+"\n")) }

// Switch makes ids the cameras that simulator udid's session uses. With no ids, the session ends.
// A camera that the session no longer names has its helper stopped, unless another simulator still uses it.
// inUse says whether that other simulator still has the webcam source selected, so stale session files of other
// simulators don't keep a helper alive.
func (e *Env) Switch(udid string, ids []string, inUse func(udid string) bool) error {
	old := ReadSession(e.Home, udid)
	if len(ids) == 0 {
		if err := os.Remove(SessionPath(e.Home, udid)); err != nil && !errors.Is(err, os.ErrNotExist) {
			return err
		}
	} else {
		var text strings.Builder
		for _, id := range ids {
			text.WriteString("camera=" + id + "\n")
		}

		if err := writeAtomic(SessionPath(e.Home, udid), []byte(text.String())); err != nil {
			return err
		}
	}

	var errs []string
	for _, id := range old {
		if slices.Contains(ids, id) {
			continue
		}

		var users []string
		for _, user := range SessionUsers(e.Home, id, udid) {
			if inUse == nil || inUse(user) {
				users = append(users, user)
			}
		}

		if len(users) > 0 {
			e.say("camera helper for %s keeps running: %d other simulator(s) still use it (%s)", id, len(users),
				strings.Join(users, ", "))
			continue
		}

		cam := CamHash(e.Home, id)
		pidFile, live, readErr := e.ReadPid(cam)
		if err := e.Stop(cam); err != nil {
			errs = append(errs, err.Error())
		} else if readErr == nil && live {
			e.say("stopped the camera helper for %s (pid %d)", id, pidFile.Pid)
		}
	}

	if len(errs) > 0 {
		return errors.New(strings.Join(errs, "; "))
	}

	return nil
}

// writeAtomic writes one of the webcam's own files (0600, its directory 0700) atomically.
func writeAtomic(p string, b []byte) error {
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		return err
	}
	return home.WriteAtomicPerm(p, b, 0o600)
}

// PidFiles lists the camera hashes that have a pid file.
func PidFiles(home string) []string {
	entries, _ := os.ReadDir(Dir(home)) // no dir: no pid files
	var out []string
	for _, entry := range entries {
		if cam, ok := strings.CutSuffix(entry.Name(), ".pid"); ok && len(cam) == 8 {
			out = append(out, cam)
		}
	}
	return out
}
