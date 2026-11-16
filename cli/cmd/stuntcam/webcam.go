// SPDX-License-Identifier: Apache-2.0
package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/avinav-b/stuntcam/cli/internal/artifacts"
	"github.com/avinav-b/stuntcam/cli/internal/home"
	"github.com/avinav-b/stuntcam/cli/internal/webcam"
)

// newWebcamEnv is the camera helper's environment (tests replace it). The home is cleaned the way arm.Vars builds
// SIMCAM_SIM_DIR, from which the engine derives the home it hashes into ring names.
var newWebcamEnv = func() *webcam.Env { return webcam.Real(filepath.Clean(home.Root()), os.Stdout) }

// webcamSpec: spec is the webcam source, with the camera NAME after "webcam:" ("" = the default camera).
func webcamSpec(spec string) (name string, ok bool) {
	if spec == "webcam" {
		return "", true
	}
	if n, ok := strings.CutPrefix(spec, "webcam:"); ok && n != "" {
		return n, true
	}
	return "", false
}

// startWebcam resolves the camera (NAME, or --front/--back: the engine feeds one camera to both positions) against
// the helper's camera list, once, makes sure a helper that serves this engine runs for it and has camera access, and
// records it as the default camera. Returns the camera's uniqueID: the source is webcam:<uniqueID> from then on.
func startWebcam(e *webcam.Env, set artifacts.Set, name, front, back string) (string, error) {
	if set.Helper == "" {
		return "", errors.New("this build has no camera helper (in a checkout: helper/build.sh; an installed stuntcam ships one)")
	}
	if !filepath.IsAbs(e.Home) {
		return "", fmt.Errorf("the host directory %s is not an absolute path", e.Home)
	}
	var names []string
	for _, n := range []string{name, front, back} {
		if n != "" {
			names = append(names, n)
		}
	}
	if len(names) == 0 {
		names = []string{""}
	}
	var cams []webcam.Camera
	for _, n := range names {
		if n != webcam.SyntheticID {
			var err error
			if cams, err = e.List(set.Helper); err != nil {
				return "", fmt.Errorf("listing the Mac's cameras: %w", err)
			}
			break
		}
	}
	var picked []webcam.Camera
	for _, n := range names {
		c, err := webcam.Resolve(cams, n)
		if err != nil {
			return "", usageError{err}
		}
		if len(picked) > 0 && picked[0].ID != c.ID {
			return "", usagef("--front, --back and webcam:NAME name different cameras (%s, %s): this engine feeds one camera to both positions", picked[0].ID, c.ID)
		}
		picked = append(picked, c)
	}
	c := picked[0]
	if c.Continuity {
		fmt.Printf("note: %s is a Continuity Camera: opening it wakes the phone\n", c.Name)
	}
	if _, err := e.Ensure(set.Helper, c); err != nil {
		return "", err
	}
	if c.ID != webcam.SyntheticID {
		if err := e.WriteDefault(c.ID); err != nil {
			return "", err
		}
	}
	return c.ID, nil
}

// controlSource is the source= line of a simulator's control file ("" if none).
func controlSource(udid string) string {
	b, _ := os.ReadFile(home.ControlFile(udid))
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(strings.TrimSpace(l), "source="); ok {
			return v
		}
	}
	return ""
}

// webcamInUse says whether another simulator still uses its webcam session: booted (if the list can be read) and its
// control file still selecting webcam.
func webcamInUse() func(udid string) bool {
	booted := map[string]bool{}
	ds, err := ctl.Devices()
	for _, d := range ds {
		if d.State == "Booted" {
			booted[d.UDID] = true
		}
	}
	return func(udid string) bool {
		if err == nil && !booted[udid] {
			return false
		}
		_, ok := webcamSpec(controlSource(udid))
		return ok
	}
}

// endWebcam ends simulator udid's webcam session: its helpers stop unless another simulator still uses them.
func endWebcam(udid string) error {
	e := newWebcamEnv()
	if len(webcam.ReadSession(e.Home, udid)) == 0 {
		return nil
	}
	return e.Switch(udid, nil, webcamInUse())
}

// webcamDoctor adds doctor's webcam lines: the helper of this build (signature, Gatekeeper, ids), every helper's state
// and permission, ring-version and shm-owner problems, and the simulators with a webcam session.
func webcamDoctor(set artifacts.Set, setErr error, say func(ok, warn bool, msg string), info func(msg string)) {
	e := newWebcamEnv()
	if setErr == nil {
		if set.Helper == "" {
			info("webcam: this build has no camera helper (in a checkout: helper/build.sh)")
		} else {
			err := e.Verify(set.Helper)
			say(err == nil, false, fmt.Sprintf("webcam helper %s: %s (codesign --verify --strict)%s", set.HelperSHA(), set.Helper, errText(err)))
			if err == nil {
				o, _ := exec.Command("/usr/bin/codesign", "-dv", set.Helper).CombinedOutput()
				id, team := field(string(o), "Identifier="), field(string(o), "TeamIdentifier=")
				g, gerr := exec.Command("/usr/sbin/spctl", "-a", "-t", "exec", "-vv", set.Helper).CombinedOutput()
				gk := "accepted"
				if gerr != nil {
					gk = "not accepted (expected for a dev build)"
				}
				if s := field(string(g), "source="); s != "" {
					gk += ", " + s
				}
				info(fmt.Sprintf("webcam helper bundle id %s, team %s; Gatekeeper (spctl -a -t exec): %s", id, team, gk))
			}
		}
	}
	any := false
	for _, cam := range webcam.PidFiles(e.Home) {
		any = true
		p, live, err := e.ReadPid(cam)
		switch {
		case err != nil:
			say(false, true, fmt.Sprintf("webcam: %s: %v", webcam.PidPath(e.Home, cam), err))
			continue
		case p.Auth == "denied":
			say(false, true, "webcam: denied ("+webcam.SettingsPath+")")
		case !live:
			info(fmt.Sprintf("webcam: the camera helper for %s exited (pid %d); `stuntcam source webcam` starts it again, `stuntcam gc` removes the pid file", p.Camera, p.Pid))
			continue
		case webcam.SharedRing(p.Rings, e.Rings) == 0:
			say(false, true, fmt.Sprintf("webcam: update stuntcam (ring %s, engine accepts %s)", rings(p.Rings), rings(e.Rings)))
		case p.Auth == "prompting":
			say(false, true, "webcam: waiting for the camera prompt: allow camera access for Stuntcam Camera in the dialog")
		}
		if live {
			say(true, false, fmt.Sprintf("webcam: helper %s (pid %d) camera %s: state %s, auth %s, ring %s", p.Sha, p.Pid, p.Camera, p.State, p.Auth, rings(p.Rings)))
		}
		for _, v := range p.Rings {
			name := webcam.ShmName(e.Uid, cam, v)
			uid, err := e.ShmOwner(name)
			if err == nil && uid != e.Uid {
				say(false, false, fmt.Sprintf("webcam: shm name %s owned by uid %d", name, uid))
			} else if errors.Is(err, syscall.EACCES) {
				say(false, false, fmt.Sprintf("webcam: shm name %s owned by another user", name))
			}
		}
	}
	if !any {
		info("webcam: no camera helper running (`stuntcam source webcam` starts one)")
	}
	sims, _ := os.ReadDir(filepath.Join(e.Home, "sims"))
	for _, s := range sims {
		if cs := webcam.ReadSession(e.Home, s.Name()); len(cs) > 0 {
			info(fmt.Sprintf("webcam: simulator %s uses camera %s (source %s)", s.Name(), strings.Join(cs, ", "), controlSource(s.Name())))
		}
	}
	if os.Getenv("SSH_CONNECTION") != "" || os.Getenv("SSH_TTY") != "" {
		info("webcam: over ssh (and on headless CI) nobody can answer the camera prompt: use `video:PATH` or `webcam:synthetic` there")
	}
}

func field(s, key string) string {
	for _, l := range strings.Split(s, "\n") {
		if v, ok := strings.CutPrefix(l, key); ok {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func rings(r []int) string {
	var s []string
	for _, v := range r {
		s = append(s, fmt.Sprintf("v%d", v))
	}
	return strings.Join(s, ",")
}

// gcHelpers cleans up after camera helpers: pid files and rings of exited ones, and helper/<sha>/ dirs that are neither
// this build's (current) nor named by a running helper, under the unpack lock.
func gcHelpers(current string, dry bool, rm func(what, p string)) {
	e := newWebcamEnv()
	root := webcam.HelperRoot(e.Home)
	if _, err := os.Stat(root); err == nil && !dry {
		unlock, err := artifacts.LockDir(root)
		if err != nil {
			fmt.Println("keeping the camera helpers:", err)
			return
		}
		defer unlock()
	}
	e.GC(map[string]bool{current: current != ""}, dry, webcam.ShmUnlink, webcam.ShmProducer, rm)
	gcStale(root, e.Now(), rm)
}
