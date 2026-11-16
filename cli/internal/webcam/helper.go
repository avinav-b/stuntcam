// SPDX-License-Identifier: Apache-2.0
package webcam

import (
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
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
			b, err := exec.Command("/usr/bin/open", append([]string{"-n", "-g", app, "--args"}, args...)...).CombinedOutput()
			if err != nil {
				return fmt.Errorf("open %s: %v %s", app, err, strings.TrimSpace(string(b)))
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

// IsHelperPath: exe is a helper bundle's executable under home/helper/<sha>/.
func IsHelperPath(home, exe string) bool {
	if exe == "" {
		return false
	}
	// the kernel reports the resolved path; resolve home the same way (both, or neither if exe doesn't resolve)
	if e, err := filepath.EvalSymlinks(exe); err == nil {
		if h, err := filepath.EvalSymlinks(home); err == nil {
			exe, home = e, h
		}
	}
	rel, err := filepath.Rel(HelperRoot(home), exe)
	if err != nil {
		return false
	}
	parts := strings.Split(filepath.ToSlash(rel), "/")
	return len(parts) == 5 && shaRe.MatchString(parts[0]) && parts[1] == AppName && strings.Join(parts[2:], "/") == ExeRel
}

func (e *Env) say(format string, a ...any) {
	if e.Out != nil {
		fmt.Fprintf(e.Out, format+"\n", a...)
	}
}

// running: pid is alive and runs a helper bundle from $SIMCAM_HOME/helper/. kill(pid, 0) alone isn't enough: after a
// helper crashes its pid file stays, and the pid can be reused by an unrelated process.
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
	for t0 := e.Now(); e.Now().Sub(t0) < e.StartWait; e.Sleep(e.Poll) {
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
		return fmt.Errorf("camera helper %s is not under %s/<sha>/: rebuild it with this SIMCAM_HOME (helper/build.sh)", app, HelperRoot(e.Home))
	}
	return e.Verify(app)
}

// ErrDenied: the user denied camera access to the helper (or it is restricted).
var ErrDenied = errors.New("camera access for Stuntcam Camera is denied: allow it in " + SettingsPath + ", then run this command again")

// ErrNoAnswer: the camera prompt wasn't answered in time.
var ErrNoAnswer = errors.New("no answer to the camera prompt: answer it (Stuntcam Camera), then run this command again")

// Ensure makes sure a helper that can serve this engine runs for camera c: it reuses a running one, replaces one that
// shares no ring version with the engine, or starts the helper at app with `open -n -g`. Then it waits for the camera
// permission (synthetic: none).
func (e *Env) Ensure(app string, c Camera) (PidFile, error) {
	cam := CamHash(e.Home, c.ID)
	prev := 0
	if p, live, err := e.ReadPid(cam); err == nil && live && p.Auth != "denied" {
		if SharedRing(p.Rings, e.Rings) > 0 {
			e.say("camera helper %s already running for %s (pid %d)", p.Sha, c.label(), p.Pid)
			return e.waitAuth(cam, p, c)
		}
		e.say("the running camera helper %s (pid %d) serves ring %s, this stuntcam's engine reads %s: replacing it", p.Sha, p.Pid, ringsText(p.Rings), ringsText(e.Rings))
		if err := e.Stop(cam); err != nil {
			return p, err
		}
		prev = p.Pid
	} else if err == nil {
		prev = p.Pid
		// a helper that was just denied exits 3 s later still holding its lock: a new one started meanwhile would quit
		for t0 := e.Now(); live && e.Now().Sub(t0) < e.StopWait; e.Sleep(e.Poll) {
			live = e.running(p.Pid)
		}
	}
	if err := e.checkApp(app); err != nil {
		return PidFile{}, err
	}
	args := []string{"--home", e.Home}
	if c.ID == SyntheticID {
		args = append(args, "--synthetic")
	} else {
		args = append(args, "--camera", c.ID)
	}
	t0 := e.Now()
	if err := e.Open(app, args); err != nil {
		return PidFile{}, err
	}
	p, err := e.waitStart(cam, t0, prev)
	if err != nil {
		return p, err
	}
	if SharedRing(p.Rings, e.Rings) == 0 {
		return p, fmt.Errorf("webcam: update stuntcam (the camera helper %s serves ring %s, this engine accepts %s)", p.Sha, ringsText(p.Rings), ringsText(e.Rings))
	}
	e.say("camera helper %s started for %s (pid %d)", p.Sha, c.label(), p.Pid)
	return e.waitAuth(cam, p, c)
}

func (c Camera) label() string {
	if c.Name != "" && c.Name != c.ID {
		return fmt.Sprintf("'%s' (%s)", c.Name, c.ID)
	}
	return c.ID
}

// waitStart waits for the pid file of a helper started at t0 (or of whichever helper won the lock meanwhile).
func (e *Env) waitStart(cam string, t0 time.Time, prev int) (PidFile, error) {
	for {
		p, live, err := e.ReadPid(cam)
		fresh := err == nil && (p.Started >= t0.Unix()-1 || p.Pid != prev)
		if fresh && (live || p.Auth == "denied") {
			return p, nil
		}
		if e.Now().Sub(t0) >= e.StartWait {
			return p, e.failure(cam, fmt.Sprintf("the camera helper didn't start within %s", e.StartWait))
		}
		e.Sleep(e.Poll)
	}
}

// waitAuth polls the pid file's auth= until the helper is authorized, denied, or PermWait passes.
func (e *Env) waitAuth(cam string, p PidFile, c Camera) (PidFile, error) {
	if c.ID == SyntheticID {
		return p, nil
	}
	t0, said := e.Now(), false
	for {
		switch p.Auth {
		case "authorized":
			if said {
				e.say("camera access allowed")
			}
			return p, nil
		case "denied":
			return p, ErrDenied
		case "prompting":
			if !said {
				e.say("allow camera access for Stuntcam Camera in the dialog (waiting up to %s)", e.PermWait)
				said = true
			}
		}
		if e.Now().Sub(t0) >= e.PermWait {
			return p, ErrNoAnswer
		}
		e.Sleep(e.Poll)
		np, live, err := e.ReadPid(cam)
		if err != nil || (!live && np.Auth != "denied") {
			return p, e.failure(cam, "the camera helper exited")
		}
		p = np
	}
}

// failure explains why a helper isn't running: another user's shm object at the ring's name, or the helper log's last line.
func (e *Env) failure(cam, what string) error {
	for _, v := range e.Rings {
		name := ShmName(e.Uid, cam, v)
		uid, err := e.ShmOwner(name)
		switch {
		case err == nil && uid != e.Uid:
			return fmt.Errorf("%s: webcam: shm name %s owned by uid %d", what, name, uid)
		case errors.Is(err, syscall.EACCES):
			return fmt.Errorf("%s: webcam: shm name %s owned by another user", what, name)
		}
	}
	lp := LogPath(e.Home, cam)
	if b, err := os.ReadFile(lp); err == nil {
		ls := strings.Split(strings.TrimSpace(string(b)), "\n")
		for i := len(ls) - 1; i >= 0 && i >= len(ls)-20; i-- {
			if strings.Contains(ls[i], "owned by uid") || strings.Contains(ls[i], "no camera") || strings.Contains(ls[i], "another helper") {
				_, msg, _ := strings.Cut(ls[i], "] ")
				return fmt.Errorf("%s: %s (log: %s)", what, msg, lp)
			}
		}
		return fmt.Errorf("%s (log: %s)", what, lp)
	}
	return errors.New(what)
}

// Stop ends the helper named by camera hash cam's pid file: SIGTERM (it clears and unlinks its ring and removes the pid
// file), then waits StopWait for it to exit. A pid that isn't a helper from $SIMCAM_HOME/helper/ is never signalled.
func (e *Env) Stop(cam string) error {
	p, live, err := e.ReadPid(cam)
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("%s: %v", PidPath(e.Home, cam), err)
	}
	if !live {
		return nil
	}
	if !e.IsHelper(p.Pid) { // re-checked right before the signal: the pid may have been reused since ReadPid
		return fmt.Errorf("pid %d in %s is not a camera helper from %s: not stopping it", p.Pid, PidPath(e.Home, cam), HelperRoot(e.Home))
	}
	if err := e.Term(p.Pid); err != nil {
		return fmt.Errorf("stopping the camera helper (pid %d): %v", p.Pid, err)
	}
	for t0 := e.Now(); e.running(p.Pid); e.Sleep(e.Poll) {
		if e.Now().Sub(t0) >= e.StopWait {
			return fmt.Errorf("the camera helper (pid %d) is still running %s after SIGTERM", p.Pid, e.StopWait)
		}
	}
	return nil
}

// WriteDefault records id as the camera a bare `webcam` source means (spec §8a).
func (e *Env) WriteDefault(id string) error { return writeAtomic(DefaultPath(e.Home), []byte(id+"\n")) }

// Switch makes ids the cameras simulator udid's session uses (none: the session ends). A camera the session no longer
// names has its helper stopped unless another simulator still uses it (inUse says whether that simulator still has
// the webcam source selected; stale session files of other simulators don't keep a helper alive).
func (e *Env) Switch(udid string, ids []string, inUse func(udid string) bool) error {
	old := ReadSession(e.Home, udid)
	if len(ids) == 0 {
		if err := os.Remove(SessionPath(e.Home, udid)); err != nil && !os.IsNotExist(err) {
			return err
		}
	} else {
		var b strings.Builder
		for _, id := range ids {
			b.WriteString("camera=" + id + "\n")
		}
		if err := writeAtomic(SessionPath(e.Home, udid), []byte(b.String())); err != nil {
			return err
		}
	}
	var errs []string
	for _, id := range old {
		if containsStr(ids, id) {
			continue
		}
		var users []string
		for _, u := range SessionUsers(e.Home, id, udid) {
			if inUse == nil || inUse(u) {
				users = append(users, u)
			}
		}
		if len(users) > 0 {
			e.say("camera helper for %s keeps running: %d other simulator(s) still use it (%s)", id, len(users), strings.Join(users, ", "))
			continue
		}
		cam := CamHash(e.Home, id)
		p, live, perr := e.ReadPid(cam)
		if err := e.Stop(cam); err != nil {
			errs = append(errs, err.Error())
		} else if perr == nil && live {
			e.say("stopped the camera helper for %s (pid %d)", id, p.Pid)
		}
	}
	if len(errs) > 0 {
		return errors.New(strings.Join(errs, "; "))
	}
	return nil
}

func containsStr(xs []string, x string) bool {
	for _, v := range xs {
		if v == x {
			return true
		}
	}
	return false
}

func writeAtomic(p string, b []byte) error {
	if err := os.MkdirAll(filepath.Dir(p), 0o700); err != nil {
		return err
	}
	tmp := p + ".tmp." + strconv.Itoa(os.Getpid())
	if err := os.WriteFile(tmp, b, 0o600); err != nil {
		return err
	}
	if err := os.Rename(tmp, p); err != nil {
		os.Remove(tmp)
		return err
	}
	return nil
}

// PidFiles lists the camera hashes that have a pid file.
func PidFiles(home string) []string {
	es, _ := os.ReadDir(Dir(home))
	var out []string
	for _, e := range es {
		if n, ok := strings.CutSuffix(e.Name(), ".pid"); ok && len(n) == 8 {
			out = append(out, n)
		}
	}
	return out
}
